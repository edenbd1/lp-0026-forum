// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Images in posts, kept on Logos Storage.
//
// A post with images carries them in two parts:
//
//  * In its signed body, a last line that every version of the forum can show
//    as text: "[Images on Logos Storage: 2, manifest sha256:<64 hex>]". It is
//    part of the bytes the author signed, so it commits to the manifest below.
//    A 0.3.x reader verifies such a post exactly like any other post and shows
//    that line; nothing else changes for it.
//  * Next to the body, in a field "att" that is not signed itself and that a
//    0.3.x reader ignores: the manifest, a small JSON document naming each
//    image's Logos Storage CID, the SHA-256 of its full bytes, its size and a
//    small preview thumbnail. A reader accepts the manifest only if its own
//    SHA-256 is the one in the signed line, so nobody but the author can
//    change it; anyone can strip it (an older peer relaying the post does), and
//    then the post shows "images unavailable" until a copy with it arrives.
//
// The full image is fetched from Logos Storage only when the reader asks for
// it, and shown only if its bytes hash to the signed SHA-256.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace forum {

class Store;

constexpr size_t kMaxImages = 4;
constexpr uint32_t kMaxImageSide = 2048;           // full image, after re-encoding
constexpr size_t kMaxImageBytes = 2u * 1024 * 1024;  // full image, after re-encoding
constexpr uint32_t kMaxThumbSide = 160;
constexpr size_t kMaxThumbBytes = 6 * 1024;
constexpr size_t kMaxManifestBytes = 40 * 1024;
constexpr size_t kMaxCidLength = 128;

struct ImageRef {
    std::string cid;     // Logos Storage content id of the full image
    std::string sha256;  // hex SHA-256 of the full image's bytes
    uint64_t bytes = 0;  // size of the full image
    uint32_t w = 0, h = 0;
    std::string type;    // "image/jpeg" or "image/png"
    std::string thumb;   // the preview's bytes (JPEG), at most kMaxThumbBytes
};

std::string sha256_hex(const std::string& bytes);

// A CID as Logos Storage writes one: 10 to 128 letters and digits.
bool is_valid_cid(const std::string& cid);

// The manifest document for `images` (1 to kMaxImages), or "" if they break a limit.
std::string make_manifest(const std::vector<ImageRef>& images);

// `text` followed by the signed line that commits to `manifest`.
std::string with_image_line(const std::string& text, const std::string& manifest);

// The image line at the end of a body, if there is one: how many images and
// the manifest's hash. `text_end` receives where the text before it ends.
struct ImageLine { size_t count = 0; std::string manifest_sha256; size_t text_end = 0; };
std::optional<ImageLine> image_line(const std::string& body);

// The body as a reader of this version sees it: without the image line.
std::string shown_body(const std::string& body);

// The images of a manifest, if it is the one `body` commits to and is within
// every limit; nothing otherwise.
std::optional<std::vector<ImageRef>> manifest_images(const std::string& body, const std::string& manifest);

// Whether downloaded bytes are the image the author signed. Nothing that
// fails this is ever shown.
bool image_bytes_match(const std::string& bytes, const ImageRef& ref);

// Images in a private group (group.h). The post and its manifest travel
// sealed like every group message, but the full image is on Logos Storage,
// which anyone can read by CID. So it is uploaded sealed: XChaCha20-Poly1305
// under a key derived from the group's seal key (BLAKE2b keyed, context
// "LFimage1"), the group's forum id bound in as associated data, padded to a
// multiple of 4 KiB. The manifest's SHA-256 and size are those of the image
// itself, checked after opening. Format: 1 | nonce (24) | ciphertext.
using GroupKey = std::array<uint8_t, 32>;
std::string seal_image(const GroupKey& seal_key, const std::string& forum_id, const std::string& plain);
std::optional<std::string> open_image(const GroupKey& seal_key, const std::string& forum_id, const std::string& sealed);
// The largest a sealed image of `plain_bytes` can be: a download above it is
// stopped.
size_t sealed_image_size(size_t plain_bytes);

// Manifests of stored posts, kept beside the posts table (a post's id is its
// signed content; the manifest is the unsigned part it commits to), and the
// CIDs of images this install published.
class Attachments {
public:
    explicit Attachments(Store& store);
    // Keep `manifest` for post `id` if none is kept yet. True if it was new.
    bool put(const std::string& id, const std::string& manifest);
    std::string get(const std::string& id) const;
    void add_own(const std::string& cid);
    std::vector<std::string> own() const;
private:
    sqlite3* db_;
};

} // namespace forum
