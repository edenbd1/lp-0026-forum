// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Every image the forum touches goes through here, both ways.
//
// Posting: the file is read (at most kMaxInputBytes), decoded under limits,
// turned upright from its EXIF orientation, copied pixel by pixel into a brand
// new image (so no text chunk, comment, EXIF, GPS, XMP or ICC profile of the
// original can follow), converted to sRGB, scaled to the forum's caps and
// encoded again as JPEG (PNG when it has transparency). The bytes uploaded to
// Logos Storage, and the thumbnail inside the post, are only ever these.
//
// Reading: a thumbnail from a post, or a full image fetched from Logos
// Storage (after its SHA-256 was checked against the signed one), is decoded
// under the same limits and re-encoded here before the view sees it, so the
// view's own decoders only ever see bytes this code wrote.
//
// The decoder: the input is capped in bytes before anything reads it; only
// PNG, JPEG and WebP are accepted, recognised from the content and never from
// a name; QImageReader's allocation limit is set, and the size the header
// announces is checked before any pixel is decoded.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace media {

constexpr qint64 kMaxInputBytes = 25ll * 1024 * 1024;  // a file picked to attach
constexpr int kMaxDecodeSide = 12000;                  // what the header may announce
constexpr qint64 kMaxDecodePixels = 40ll * 1000 * 1000;
constexpr int kAllocationLimitMb = 192;
constexpr int kDisplaySide = 1600;                     // what the view is given of a full image

struct Prepared {
    QByteArray full;   // re-encoded, at most forum::kMaxImageBytes
    QString type;      // "image/jpeg" or "image/png"
    int w = 0, h = 0;
    QByteArray thumb;  // JPEG, at most forum::kMaxThumbSide wide or high, forum::kMaxThumbBytes
    QString error;     // non-empty: refused
};

// For posting: a local file, picked by the user.
Prepared prepareFile(const QString& path);
Prepared prepareBytes(const QByteArray& input);

// The hardened decoder. Empty image and `error` set if refused.
QImage decode(const QByteArray& bytes, qint64 maxBytes, QString* error);

// For reading: bytes from the network, re-encoded for the view (at most
// `maxSide` on a side). Empty if refused.
QByteArray displayCopy(const QByteArray& bytes, qint64 maxBytes, int maxSide, QString* mime);

QString dataUrl(const QByteArray& bytes, const QString& mime);

} // namespace media
