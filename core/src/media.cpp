// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/media.h"

#include "forum/post.h"
#include "forum/store.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

#include <cctype>

namespace forum {
namespace {

using json = nlohmann::json;

constexpr const char* kLineHead = "\n\n[Images on Logos Storage: ";
constexpr const char* kLineMid = ", manifest sha256:";
// "\n\n[Images on Logos Storage: " + one digit + ", manifest sha256:" + 64 hex + "]"
constexpr size_t kHeadLen = 28, kMidLen = 18, kLineLen = kHeadLen + 1 + kMidLen + 64 + 1;

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string b64(const std::string& d) {
    std::string out(sodium_base64_encoded_len(d.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(d.data()), d.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(out.find('\0'));
    return out;
}

// Decodes at most `max` bytes; nothing if the text is not base64 or longer.
std::optional<std::string> unb64(const std::string& s, size_t max) {
    if (s.size() > (max + 2) / 3 * 4) return std::nullopt;
    std::string out(max, '\0');
    size_t n = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), s.data(), s.size(), nullptr, &n,
                          nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        return std::nullopt;
    out.resize(n);
    return out;
}

uint64_t u64_at(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
}
std::string str_at(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS post_att (id TEXT PRIMARY KEY, manifest TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS own_images (cid TEXT PRIMARY KEY);
)sql";

constexpr size_t kImagePad = 4096;

std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> image_key(const GroupKey& seal_key) {
    std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> k{};
    static const char ctx[] = "LFimage1";
    crypto_generichash(k.data(), k.size(), reinterpret_cast<const uint8_t*>(ctx), sizeof ctx - 1, seal_key.data(), seal_key.size());
    return k;
}
std::string image_ad(const std::string& forum_id) { return "logos-forum:image:v1:" + forum_id; }

} // namespace

size_t sealed_image_size(size_t plain_bytes) {
    return 1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + (plain_bytes / kImagePad + 1) * kImagePad +
           crypto_aead_xchacha20poly1305_ietf_ABYTES;
}

std::string seal_image(const GroupKey& seal_key, const std::string& forum_id, const std::string& plain) {
    if (plain.size() > kMaxImageBytes) return {};
    std::vector<uint8_t> padded(plain.size() + kImagePad);
    std::copy(plain.begin(), plain.end(), padded.begin());
    size_t padded_len = 0;
    if (sodium_pad(&padded_len, padded.data(), plain.size(), kImagePad, padded.size()) != 0) return {};
    const auto key = image_key(seal_key);
    const std::string ad = image_ad(forum_id);
    std::string out(1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + padded_len + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    out[0] = 1;
    auto* nonce = reinterpret_cast<uint8_t*>(out.data() + 1);
    randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(nonce + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &clen, padded.data(), padded_len,
                                               reinterpret_cast<const uint8_t*>(ad.data()), ad.size(), nullptr, nonce, key.data());
    sodium_memzero(padded.data(), padded.size());
    return out;
}

std::optional<std::string> open_image(const GroupKey& seal_key, const std::string& forum_id, const std::string& sealed) {
    const size_t head = 1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (sealed.size() < head + crypto_aead_xchacha20poly1305_ietf_ABYTES || sealed[0] != 1 ||
        sealed.size() > sealed_image_size(kMaxImageBytes))
        return std::nullopt;
    const auto key = image_key(seal_key);
    const std::string ad = image_ad(forum_id);
    const auto* nonce = reinterpret_cast<const uint8_t*>(sealed.data() + 1);
    std::vector<uint8_t> plain(sealed.size() - head);
    unsigned long long plen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &plen, nullptr, nonce + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
                                                   sealed.size() - head, reinterpret_cast<const uint8_t*>(ad.data()), ad.size(),
                                                   nonce, key.data()) != 0)
        return std::nullopt;
    size_t unpadded = 0;
    if (sodium_unpad(&unpadded, plain.data(), static_cast<size_t>(plen), kImagePad) != 0) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(plain.data()), unpadded);
}

std::string sha256_hex(const std::string& bytes) {
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    return to_hex(h, sizeof h);
}

bool is_valid_cid(const std::string& cid) {
    if (cid.size() < 10 || cid.size() > kMaxCidLength) return false;
    for (char c : cid)
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string make_manifest(const std::vector<ImageRef>& images) {
    if (images.empty() || images.size() > kMaxImages) return {};
    json list = json::array();
    for (const auto& i : images) {
        if (!is_valid_cid(i.cid) || !is_hex64(i.sha256) || i.bytes == 0 || i.bytes > kMaxImageBytes || i.w == 0 ||
            i.h == 0 || i.w > kMaxImageSide || i.h > kMaxImageSide || i.thumb.empty() || i.thumb.size() > kMaxThumbBytes ||
            (i.type != "image/jpeg" && i.type != "image/png"))
            return {};
        list.push_back({{"cid", i.cid}, {"sha256", i.sha256}, {"bytes", i.bytes}, {"w", i.w}, {"h", i.h},
                        {"type", i.type}, {"thumb", b64(i.thumb)}});
    }
    std::string out = json{{"v", 1}, {"images", list}}.dump();
    return out.size() <= kMaxManifestBytes ? out : std::string();
}

std::string with_image_line(const std::string& text, const std::string& manifest) {
    size_t n = 0;
    const json m = json::parse(manifest, nullptr, false);
    if (m.is_object() && m.contains("images") && m["images"].is_array()) n = m["images"].size();
    return text + kLineHead + std::to_string(n) + kLineMid + sha256_hex(manifest) + "]";
}

std::optional<ImageLine> image_line(const std::string& body) {
    if (body.size() < kLineLen) return std::nullopt;
    const size_t at = body.size() - kLineLen;
    if (body.compare(at, kHeadLen, kLineHead) != 0) return std::nullopt;
    const char d = body[at + kHeadLen];
    if (d < '1' || d > '0' + static_cast<char>(kMaxImages)) return std::nullopt;
    if (body.compare(at + kHeadLen + 1, kMidLen, kLineMid) != 0 || body.back() != ']') return std::nullopt;
    const std::string hash = body.substr(at + kHeadLen + 1 + kMidLen, 64);
    if (!is_hex64(hash)) return std::nullopt;
    return ImageLine{static_cast<size_t>(d - '0'), hash, at};
}

std::string shown_body(const std::string& body) {
    const auto line = image_line(body);
    return line ? body.substr(0, line->text_end) : body;
}

std::optional<std::vector<ImageRef>> manifest_images(const std::string& body, const std::string& manifest) {
    if (manifest.empty() || manifest.size() > kMaxManifestBytes) return std::nullopt;
    const auto line = image_line(body);
    if (!line || sha256_hex(manifest) != line->manifest_sha256) return std::nullopt;
    const json m = json::parse(manifest, nullptr, false);
    if (!m.is_object() || u64_at(m, "v") != 1) return std::nullopt;
    const auto it = m.find("images");
    if (it == m.end() || !it->is_array() || it->size() != line->count) return std::nullopt;
    std::vector<ImageRef> out;
    for (const auto& j : *it) {
        if (!j.is_object()) return std::nullopt;
        ImageRef r;
        r.cid = str_at(j, "cid");
        r.sha256 = str_at(j, "sha256");
        r.bytes = u64_at(j, "bytes");
        const uint64_t w = u64_at(j, "w"), h = u64_at(j, "h");
        r.type = str_at(j, "type");
        if (!is_valid_cid(r.cid) || !is_hex64(r.sha256) || r.bytes == 0 || r.bytes > kMaxImageBytes || w == 0 || h == 0 ||
            w > kMaxImageSide || h > kMaxImageSide || (r.type != "image/jpeg" && r.type != "image/png"))
            return std::nullopt;
        r.w = static_cast<uint32_t>(w);
        r.h = static_cast<uint32_t>(h);
        const auto thumb = unb64(str_at(j, "thumb"), kMaxThumbBytes);
        if (!thumb || thumb->empty()) return std::nullopt;
        r.thumb = *thumb;
        out.push_back(std::move(r));
    }
    return out;
}

bool image_bytes_match(const std::string& bytes, const ImageRef& ref) {
    return !bytes.empty() && bytes.size() <= kMaxImageBytes && bytes.size() == ref.bytes && sha256_hex(bytes) == ref.sha256;
}

Attachments::Attachments(Store& store) : db_(store.handle()) {
    sqlite3_exec(db_, kSchema, nullptr, nullptr, nullptr);
}

bool Attachments::put(const std::string& id, const std::string& manifest) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO post_att VALUES (?,?)", -1, &s, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, manifest.data(), static_cast<int>(manifest.size()), SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    sqlite3_finalize(s);
    return ok;
}

std::string Attachments::get(const std::string& id) const {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT manifest FROM post_att WHERE id=?", -1, &s, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(s, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    std::string out;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const auto* t = sqlite3_column_text(s, 0);
        if (t) out.assign(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    return out;
}

void Attachments::add_own(const std::string& cid) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO own_images VALUES (?)", -1, &s, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, cid.data(), static_cast<int>(cid.size()), SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

std::vector<std::string> Attachments::own() const {
    std::vector<std::string> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT cid FROM own_images", -1, &s, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const auto* t = sqlite3_column_text(s, 0);
        if (t) out.emplace_back(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    return out;
}

} // namespace forum
