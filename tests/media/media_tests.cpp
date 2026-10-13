// SPDX-License-Identifier: MIT OR Apache-2.0
// The image pipeline, each check named for what it guarantees. Every input is
// built here, byte by byte where it matters (EXIF with GPS, XMP, a comment, an
// ICC profile; PNG text chunks; headers announcing huge images).
#include "image_pipeline.h"

#include "forum/media.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

static int failures = 0, ran = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
struct T { const char* name; std::function<void()> f; };
static std::vector<T>& all() { static std::vector<T> v; return v; }
#define TEST(n) static void n(); static const bool reg_##n = (all().push_back({#n, n}), true); static void n()
static QString g_out;  // where to keep before/after files, if asked

static void keep(const char* name, const QByteArray& bytes) {
    if (g_out.isEmpty()) return;
    QFile f(g_out + QLatin1Char('/') + QLatin1String(name));
    if (f.open(QIODevice::WriteOnly)) f.write(bytes);
}

static QByteArray save(const QImage& img, const char* fmt, int q = -1) {
    QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, fmt); if (q >= 0) w.setQuality(q);
    w.write(img);
    return b;
}

// ── little-endian TIFF/EXIF writer, enough for the tags below ────────────────
struct Le {
    QByteArray b;
    void u16(int v) { b.append(char(v & 255)); b.append(char((v >> 8) & 255)); }
    void u32(quint32 v) { for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 255)); }
};
static const char kMake[] = "SecretCam";
static const char kGpsMarkerText[] = "48.857N 2.295E";      // also in the XMP
// GPS latitude 48°51'24.22" N, longitude 2°17'40.26" E as EXIF rationals.
static const quint32 kLat[6] = {48, 1, 51, 1, 2422, 100}, kLon[6] = {2, 1, 17, 1, 4026, 100};

static QByteArray exif(int orientation) {
    Le t;
    t.b.append("II"); t.u16(42); t.u32(8);
    // IFD0 at 8: Make, Orientation, GPS pointer.
    const quint32 ifd0 = 8, n0 = 3, ifd0End = ifd0 + 2 + n0 * 12 + 4;
    const quint32 makeAt = ifd0End, gpsAt = makeAt + sizeof kMake;   // 10 bytes of "SecretCam\0"
    const quint32 n1 = 4, gpsEnd = gpsAt + 2 + n1 * 12 + 4, latAt = gpsEnd, lonAt = latAt + 24;
    t.u16(n0);
    t.u16(0x010F); t.u16(2); t.u32(sizeof kMake); t.u32(makeAt);
    t.u16(0x0112); t.u16(3); t.u32(1); t.u16(orientation); t.u16(0);
    t.u16(0x8825); t.u16(4); t.u32(1); t.u32(gpsAt);
    t.u32(0);
    t.b.append(kMake, sizeof kMake);
    t.u16(n1);
    t.u16(0x0001); t.u16(2); t.u32(2); t.b.append("N\0\0\0", 4);
    t.u16(0x0002); t.u16(5); t.u32(3); t.u32(latAt);
    t.u16(0x0003); t.u16(2); t.u32(2); t.b.append("E\0\0\0", 4);
    t.u16(0x0004); t.u16(5); t.u32(3); t.u32(lonAt);
    t.u32(0);
    for (quint32 v : kLat) t.u32(v);
    for (quint32 v : kLon) t.u32(v);
    return QByteArray("Exif\0\0", 6) + t.b;
}
static QByteArray gpsRationals() { Le t; for (quint32 v : kLat) t.u32(v); return t.b; }

static QByteArray segment(int marker, const QByteArray& payload) {
    QByteArray s; s.append(char(0xFF)); s.append(char(marker));
    const int len = payload.size() + 2;
    s.append(char(len >> 8)); s.append(char(len & 255));
    return s + payload;
}

// A JPEG as a phone writes one: EXIF with orientation and GPS, XMP, a comment
// and an ICC profile, all after SOI.
static QByteArray phoneJpeg(int orientation, int w = 64, int h = 32) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.setPixel(x, y, x < w / 2 ? qRgb(230, 20, 20) : qRgb(20, 20, 230));
    const QByteArray plain = save(img, "jpeg", 95);
    const QByteArray xmp = QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) +
        QByteArray("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><exif:GPSLatitude>") + kGpsMarkerText + "</exif:GPSLatitude></x:xmpmeta>";
    const QByteArray icc = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    const QByteArray app2 = QByteArray("ICC_PROFILE\0", 12) + char(1) + char(1) + icc;
    return plain.left(2) + segment(0xE1, exif(orientation)) + segment(0xE1, xmp) + segment(0xE2, app2) +
           segment(0xFE, "secret comment: 12 rue de la Paix") + plain.mid(2);
}

static bool has(const QByteArray& hay, const QByteArray& needle) { return hay.indexOf(needle) >= 0; }

// Every JPEG marker segment between SOI and SOS.
static std::vector<int> markers(const QByteArray& j) {
    std::vector<int> out;
    int i = 2;
    while (i + 4 <= j.size() && uchar(j[i]) == 0xFF) {
        const int m = uchar(j[i + 1]);
        out.push_back(m);
        if (m == 0xDA) break;
        i += 2 + ((uchar(j[i + 2]) << 8) | uchar(j[i + 3]));
    }
    return out;
}

// PNG chunk types, in order.
static std::vector<QByteArray> chunks(const QByteArray& p) {
    std::vector<QByteArray> out;
    int i = 8;
    while (i + 8 <= p.size()) {
        const quint32 len = (uchar(p[i]) << 24) | (uchar(p[i + 1]) << 16) | (uchar(p[i + 2]) << 8) | uchar(p[i + 3]);
        out.push_back(p.mid(i + 4, 4));
        i += 12 + int(len);
    }
    return out;
}

static quint32 crc32(const QByteArray& d) {
    quint32 c = 0xFFFFFFFFu;
    for (uchar b : d) { c ^= b; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c ^ 0xFFFFFFFFu;
}
// A PNG whose header announces w × h; its one IDAT holds almost nothing.
static QByteArray pngClaiming(quint32 w, quint32 h) {
    auto be = [](quint32 v) { QByteArray b; for (int i = 3; i >= 0; --i) b.append(char((v >> (8 * i)) & 255)); return b; };
    auto chunk = [&](const char* type, const QByteArray& data) {
        const QByteArray td = QByteArray(type, 4) + data;
        return be(data.size()) + td + be(crc32(td));
    };
    const QByteArray ihdr = be(w) + be(h) + QByteArray("\x08\x02\x00\x00\x00", 5);
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) +
           chunk("IDAT", QByteArray("\x78\x9c\x03\x00\x00\x00\x00\x01", 8)) + chunk("IEND", {});
}

// ── tests ────────────────────────────────────────────────────────────────────

TEST(a_phone_jpeg_loses_its_gps_exif_xmp_comment_and_icc_profile) {
    const QByteArray in = phoneJpeg(1);
    keep("phone-in.jpg", in);
    // The input really carries all of it, and Qt sees the ICC profile.
    CHECK(has(in, "Exif") && has(in, kMake) && has(in, gpsRationals()) && has(in, "xmpmeta") && has(in, "ICC_PROFILE") &&
          has(in, "secret comment"));
    const auto p = media::prepareBytes(in);
    CHECK(p.error.isEmpty());
    keep("phone-out.jpg", p.full);
    keep("phone-thumb.jpg", p.thumb);
    for (const QByteArray& out : {p.full, p.thumb}) {
        CHECK(out.startsWith("\xFF\xD8"));
        CHECK(!has(out, "Exif") && !has(out, kMake) && !has(out, gpsRationals()) && !has(out, kGpsMarkerText));
        CHECK(!has(out, "xmpmeta") && !has(out, "ns.adobe.com") && !has(out, "ICC_PROFILE") && !has(out, "secret comment"));
        // Only JFIF (APP0), tables and frame markers: no APP1…APP15, no COM.
        for (int m : markers(out)) CHECK(m == 0xE0 || m == 0xDB || m == 0xC4 || m == 0xC0 || m == 0xC2 || m == 0xDD || m == 0xDA);
        QImageReader r(new QBuffer(new QByteArray(out)));
        CHECK(r.text(QStringLiteral("Make")).isEmpty());
        QImage back; r.read(&back);
        CHECK(!back.colorSpace().isValid() || back.colorSpace() == QColorSpace(QColorSpace::SRgb));
    }
    CHECK(p.type == QStringLiteral("image/jpeg") && p.w == 64 && p.h == 32);
}

TEST(exif_orientation_is_applied_before_the_metadata_goes) {
    // Orientation 6: the camera was turned; the picture is shown turned 90°
    // clockwise. Left (red) ends up on top, and the size is swapped.
    const auto p = media::prepareBytes(phoneJpeg(6));
    CHECK(p.error.isEmpty() && p.w == 32 && p.h == 64);
    const QImage out = QImage::fromData(p.full);
    CHECK(out.width() == 32 && out.height() == 64);
    CHECK(qRed(out.pixel(16, 8)) > 150 && qBlue(out.pixel(16, 8)) < 100);
    CHECK(qBlue(out.pixel(16, 56)) > 150 && qRed(out.pixel(16, 56)) < 100);
    // Orientation 3: upside down. Left and right swap.
    const auto q = media::prepareBytes(phoneJpeg(3));
    const QImage o3 = QImage::fromData(q.full);
    CHECK(q.w == 64 && q.h == 32 && qBlue(o3.pixel(8, 16)) > 150 && qRed(o3.pixel(56, 16)) > 150);
}

TEST(png_text_icc_and_exif_chunks_are_not_carried_over) {
    QImage img(50, 40, QImage::Format_ARGB32);
    img.fill(qRgba(10, 200, 30, 128));                      // transparent: stays PNG
    img.setText(QStringLiteral("Comment"), QStringLiteral("secret-png-text"));
    img.setText(QStringLiteral("GPS"), QString::fromLatin1(kGpsMarkerText));
    img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QByteArray in = save(img, "png");
    keep("text-in.png", in);
    CHECK(has(in, "tEXt") && has(in, "secret-png-text") && has(in, "iCCP"));
    const auto p = media::prepareBytes(in);
    keep("text-out.png", p.full);
    CHECK(p.error.isEmpty() && p.type == QStringLiteral("image/png"));
    CHECK(!has(p.full, "secret-png-text") && !has(p.full, kGpsMarkerText));
    for (const auto& c : chunks(p.full))
        CHECK(c != "tEXt" && c != "iTXt" && c != "zTXt" && c != "iCCP" && c != "eXIf" && c != "sRGB" && c != "gAMA" && c != "cHRM");
    // An opaque PNG becomes a JPEG.
    QImage opaque(50, 40, QImage::Format_RGB32);
    opaque.fill(qRgb(1, 2, 3));
    opaque.setText(QStringLiteral("Author"), QStringLiteral("secret-author"));
    const auto o = media::prepareBytes(save(opaque, "png"));
    CHECK(o.type == QStringLiteral("image/jpeg") && !has(o.full, "secret-author"));
}

TEST(only_png_jpeg_and_webp_are_decoded_whatever_the_name) {
    QImage img(20, 20, QImage::Format_RGB32);
    img.fill(Qt::red);
    QString why;
    CHECK(media::decode(save(img, "bmp"), media::kMaxInputBytes, &why).isNull() && why.contains(QLatin1String("not a PNG")));
    const QByteArray gif = QByteArray::fromHex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b");
    CHECK(media::decode(gif, media::kMaxInputBytes, &why).isNull());
    CHECK(media::decode("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>", media::kMaxInputBytes, &why).isNull());
    if (QImageWriter::supportedImageFormats().contains("tiff"))
        CHECK(media::decode(save(img, "tiff"), media::kMaxInputBytes, &why).isNull());
    CHECK(media::decode(QByteArray(4096, 'x'), media::kMaxInputBytes, &why).isNull());
    const QByteArray jpg = save(img, "jpeg");
    CHECK(media::decode(jpg.left(jpg.size() / 3), media::kMaxInputBytes, &why).isNull() || true);  // truncated: refused or partial, never a crash
    CHECK(!media::decode(jpg, media::kMaxInputBytes, &why).isNull());
    CHECK(!media::decode(save(img, "png"), media::kMaxInputBytes, &why).isNull());
    if (QImageWriter::supportedImageFormats().contains("webp")) {
        const auto w = media::prepareBytes(save(img, "webp"));
        CHECK(w.error.isEmpty() && w.type == QStringLiteral("image/jpeg"));
    } else {
        std::printf("  (no WebP plugin in this Qt: WebP not checked)\n");
    }
}

TEST(input_is_capped_in_bytes_before_any_decoding) {
    QString why;
    CHECK(media::decode(QByteArray(media::kMaxInputBytes + 1, '\0'), media::kMaxInputBytes, &why).isNull());
    CHECK(why.contains(QLatin1String("too large")));
    const auto p = media::prepareBytes(QByteArray(media::kMaxInputBytes + 1, '\xFF'));
    CHECK(!p.error.isEmpty() && p.full.isEmpty());
}

TEST(a_header_announcing_too_many_pixels_is_refused_before_decoding) {
    QString why;
    CHECK(media::decode(pngClaiming(30000, 30000), media::kMaxInputBytes, &why).isNull() && why.contains(QLatin1String("too many pixels")));
    CHECK(media::decode(pngClaiming(8000, 6000), media::kMaxInputBytes, &why).isNull() && why.contains(QLatin1String("too many pixels")));
    CHECK(media::decode(pngClaiming(100000, 10), media::kMaxInputBytes, &why).isNull() && why.contains(QLatin1String("too many pixels")));
    // And a JPEG header lying about its size the same way.
    QImage img(16, 16, QImage::Format_RGB32); img.fill(Qt::gray);
    QByteArray j = save(img, "jpeg");
    const int sof = j.indexOf("\xFF\xC0");
    CHECK(sof > 0);
    j[sof + 5] = char(0xFF); j[sof + 6] = char(0xFF); j[sof + 7] = char(0xFF); j[sof + 8] = char(0xFF);   // 65535 × 65535
    CHECK(media::decode(j, media::kMaxInputBytes, &why).isNull());
}

TEST(the_full_image_and_the_thumbnail_are_within_their_caps) {
    QImage noise(4000, 3000, QImage::Format_RGB32);
    auto* rng = QRandomGenerator::global();
    for (int y = 0; y < noise.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(noise.scanLine(y));
        for (int x = 0; x < noise.width(); ++x) line[x] = 0xFF000000u | rng->generate();
    }
    const QByteArray in = save(noise, "jpeg", 92);           // ~12 MP of noise, under the input cap
    CHECK(in.size() <= media::kMaxInputBytes);
    const auto p = media::prepareBytes(in);
    if (!p.error.isEmpty()) std::printf("  refused: %s\n", qPrintable(p.error));
    CHECK(p.error.isEmpty());
    CHECK(p.w <= int(forum::kMaxImageSide) && p.h <= int(forum::kMaxImageSide) && p.w > 0);
    CHECK(p.full.size() <= qint64(forum::kMaxImageBytes));
    const QImage t = QImage::fromData(p.thumb);
    CHECK(t.width() <= int(forum::kMaxThumbSide) && t.height() <= int(forum::kMaxThumbSide));
    CHECK(p.thumb.size() <= qint64(forum::kMaxThumbBytes));
    // And the result is accepted by the core's manifest limits.
    forum::ImageRef r{"zDvZRwzm1aWdqbbPPbdjQKtX2oZyTFGz6vVyqUEe8tqb3zoP5xaa", std::string(64, 'a'), quint64(p.full.size()),
                      quint32(p.w), quint32(p.h), p.type.toStdString(), p.thumb.toStdString()};
    CHECK(r.bytes <= forum::kMaxImageBytes && r.thumb.size() <= forum::kMaxThumbBytes);
}

TEST(what_the_view_is_given_is_always_re_encoded_here) {
    // A thumbnail or a full image from the network is decoded under the same
    // limits and written again: the view never decodes the sender's bytes.
    const QByteArray in = phoneJpeg(1, 400, 200);
    QString mime;
    const QByteArray shown = media::displayCopy(in, forum::kMaxImageBytes, 100, &mime);
    CHECK(mime == QStringLiteral("image/jpeg") && shown.startsWith("\xFF\xD8") && shown != in);
    CHECK(!has(shown, "Exif") && !has(shown, "xmpmeta") && !has(shown, "ICC_PROFILE") && !has(shown, "secret comment"));
    const QImage s = QImage::fromData(shown);
    CHECK(s.width() == 100 && s.height() == 50);
    CHECK(media::displayCopy(QByteArray(100, 'x'), forum::kMaxImageBytes, 100, &mime).isEmpty());
    CHECK(media::displayCopy(in, 100, 100, &mime).isEmpty());                 // over the byte cap given
    CHECK(media::displayCopy(pngClaiming(30000, 30000), forum::kMaxImageBytes, 100, &mime).isEmpty());
    CHECK(media::dataUrl(shown, mime).startsWith(QLatin1String("data:image/jpeg;base64,/9j/")));
}

TEST(only_a_regular_file_is_read) {
    QTemporaryDir dir;
    CHECK(!media::prepareFile(dir.path()).error.isEmpty());                    // a directory
    CHECK(!media::prepareFile(dir.path() + QStringLiteral("/missing.jpg")).error.isEmpty());
#ifndef _WIN32
    CHECK(!media::prepareFile(QStringLiteral("/dev/zero")).error.isEmpty());   // a device: never read
    const QByteArray fifo = (dir.path() + QStringLiteral("/pipe.jpg")).toLocal8Bit();
    CHECK(mkfifo(fifo.constData(), 0600) == 0);
    CHECK(!media::prepareFile(QString::fromLocal8Bit(fifo)).error.isEmpty()); // a pipe: would block, never opened
#endif
    QFile f(dir.path() + QStringLiteral("/photo.png"));
    f.open(QIODevice::WriteOnly); f.write(phoneJpeg(1)); f.close();           // a JPEG named .png: read as what it is
    const auto p = media::prepareFile(f.fileName());
    CHECK(p.error.isEmpty() && p.type == QStringLiteral("image/jpeg") && !has(p.full, "Exif"));
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc > 1) { g_out = QString::fromLocal8Bit(argv[1]); QDir().mkpath(g_out); }
    for (auto& t : all()) {
        const int before = failures;
        t.f();
        ++ran;
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d failure(s)\n", ran, failures);
    return failures ? 1 : 0;
}
