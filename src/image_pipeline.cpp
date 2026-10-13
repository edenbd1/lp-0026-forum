// SPDX-License-Identifier: MIT OR Apache-2.0
#include "image_pipeline.h"

#include "forum/media.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

#include <cstring>

namespace media {
namespace {

// A new image holding only the pixels: nothing of the decoded image's text
// keys, colour space or other attributes is carried over.
QImage freshCopy(const QImage& decoded) {
    QImage src = decoded;
    if (src.colorSpace().isValid() && src.colorSpace() != QColorSpace(QColorSpace::SRgb))
        src.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    const QImage::Format fmt = src.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32;
    src = src.convertToFormat(fmt);
    QImage out(src.width(), src.height(), fmt);
    if (out.isNull()) return {};
    for (int y = 0; y < src.height(); ++y)
        std::memcpy(out.scanLine(y), src.constScanLine(y), static_cast<size_t>(src.width()) * 4);
    return out;
}

bool hasTransparency(const QImage& img) {
    if (!img.hasAlphaChannel()) return false;
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(line[x]) != 255) return true;
    }
    return false;
}

QImage flatten(const QImage& img, QColor under) {
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(under);
    QPainter p(&out);
    p.drawImage(0, 0, img);
    p.end();
    return out;
}

QImage fit(const QImage& img, int side) {
    if (img.width() <= side && img.height() <= side) return img;
    return img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QByteArray encode(const QImage& img, const char* format, int quality) {
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, format);
    if (quality >= 0) w.setQuality(quality);
    if (!w.write(img)) return {};
    return out;
}

// JPEG under `maxBytes`: lower quality first, then smaller.
QByteArray jpegUnder(QImage img, qint64 maxBytes, int quality, int minQuality, int* w, int* h) {
    for (int round = 0; round < 12 && !img.isNull(); ++round) {
        for (int q = quality; q >= minQuality; q -= 10) {
            const QByteArray b = encode(img, "jpeg", q);
            if (!b.isEmpty() && b.size() <= maxBytes) {
                *w = img.width();
                *h = img.height();
                return b;
            }
        }
        img = img.scaled(img.width() * 4 / 5, img.height() * 4 / 5, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {};
}

} // namespace

QImage decode(const QByteArray& bytes, qint64 maxBytes, QString* error) {
    auto refuse = [&](const QString& why) {
        if (error) *error = why;
        return QImage();
    };
    if (bytes.isEmpty()) return refuse(QStringLiteral("empty"));
    if (bytes.size() > maxBytes) return refuse(QStringLiteral("too large (over %1 bytes)").arg(maxBytes));
    QByteArray copy = bytes;
    QBuffer buf(&copy);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setDecideFormatFromContent(true);
    reader.setAllocationLimit(kAllocationLimitMb);
    reader.setAutoTransform(true);  // EXIF orientation first: the pixels come out upright
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "webp")
        return refuse(QStringLiteral("not a PNG, JPEG or WebP image"));
    const QSize size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) return refuse(QStringLiteral("no readable size"));
    if (size.width() > kMaxDecodeSide || size.height() > kMaxDecodeSide ||
        static_cast<qint64>(size.width()) * size.height() > kMaxDecodePixels)
        return refuse(QStringLiteral("too many pixels (%1 × %2)").arg(size.width()).arg(size.height()));
    QImage img;
    if (!reader.read(&img) || img.isNull()) return refuse(QStringLiteral("could not be decoded: %1").arg(reader.errorString()));
    return img;
}

Prepared prepareFile(const QString& path) {
    Prepared p;
    const QFileInfo fi(path);
    // A regular file only: never a device, a pipe or a directory.
    if (!fi.exists() || !fi.isFile()) {
        p.error = QStringLiteral("not a file");
        return p;
    }
    if (fi.size() > kMaxInputBytes) {
        p.error = QStringLiteral("the file is over %1 MB").arg(kMaxInputBytes / (1024 * 1024));
        return p;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        p.error = QStringLiteral("the file cannot be read");
        return p;
    }
    const QByteArray bytes = f.read(kMaxInputBytes + 1);  // bounded even if the file grew meanwhile
    return prepareBytes(bytes);
}

Prepared prepareBytes(const QByteArray& input) {
    Prepared p;
    QString why;
    const QImage decoded = decode(input, kMaxInputBytes, &why);
    if (decoded.isNull()) {
        p.error = why;
        return p;
    }
    const QImage clean = freshCopy(decoded);
    if (clean.isNull()) {
        p.error = QStringLiteral("out of memory");
        return p;
    }
    const QImage full = fit(clean, static_cast<int>(forum::kMaxImageSide));
    const qint64 cap = static_cast<qint64>(forum::kMaxImageBytes);
    if (hasTransparency(full)) {
        const QByteArray png = encode(full, "png", -1);
        if (!png.isEmpty() && png.size() <= cap) {
            p.full = png;
            p.type = QStringLiteral("image/png");
            p.w = full.width();
            p.h = full.height();
        }
    }
    if (p.full.isEmpty()) {
        p.full = jpegUnder(hasTransparency(full) ? flatten(full, Qt::white) : full, cap, 88, 48, &p.w, &p.h);
        p.type = QStringLiteral("image/jpeg");
    }
    if (p.full.isEmpty()) {
        p.error = QStringLiteral("could not be made small enough");
        return p;
    }
    int tw = 0, th = 0;
    const QImage small = fit(flatten(clean, QColor(0x20, 0x24, 0x2b)), static_cast<int>(forum::kMaxThumbSide));
    p.thumb = jpegUnder(small, static_cast<qint64>(forum::kMaxThumbBytes), 75, 25, &tw, &th);
    if (p.thumb.isEmpty()) p.error = QStringLiteral("no thumbnail could be made");
    return p;
}

QByteArray displayCopy(const QByteArray& bytes, qint64 maxBytes, int maxSide, QString* mime) {
    QString why;
    const QImage decoded = decode(bytes, maxBytes, &why);
    if (decoded.isNull()) return {};
    const QImage clean = fit(freshCopy(decoded), maxSide);
    if (clean.isNull()) return {};
    if (hasTransparency(clean)) {
        if (mime) *mime = QStringLiteral("image/png");
        return encode(clean, "png", -1);
    }
    if (mime) *mime = QStringLiteral("image/jpeg");
    return encode(clean, "jpeg", 90);
}

QString dataUrl(const QByteArray& bytes, const QString& mime) {
    if (bytes.isEmpty()) return {};
    return QStringLiteral("data:") + mime + QStringLiteral(";base64,") + QString::fromLatin1(bytes.toBase64());
}

} // namespace media
