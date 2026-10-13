// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/group.h"

#include <sodium.h>
#include <sqlite3.h>

#include <cctype>
#include <cstring>

namespace forum {
namespace {

constexpr char kKdfContext[crypto_kdf_CONTEXTBYTES + 1] = "LFgroup1";
constexpr uint64_t kTopicKey = 1, kSealKey = 2, kForumKey = 3;
constexpr uint8_t kInviteVersion = 1, kSealVersion = 1;
constexpr size_t kPad = 1024;
const std::string kAdDomain = "logos-forum:group:v1:";

std::array<uint8_t, 32> subkey(const GroupSecret& s, uint64_t id) {
    std::array<uint8_t, 32> out{};
    crypto_kdf_derive_from_key(out.data(), out.size(), id, kKdfContext, s.data());
    return out;
}

std::string b64(const unsigned char* d, size_t n, int variant) {
    std::string out(sodium_base64_encoded_len(n, variant), '\0');
    sodium_bin2base64(out.data(), out.size(), d, n, variant);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::string> unb64(const std::string& in, int variant, size_t max) {
    std::string out(in.size(), '\0');
    size_t n = 0;
    if (in.empty() || in.size() > max ||
        sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), in.data(), in.size(), nullptr, &n,
                          nullptr, variant) != 0)
        return std::nullopt;
    out.resize(n);
    return out;
}

} // namespace

std::optional<Group> Group::create(const std::string& name) {
    const std::string shown = display_alias(name);
    if (shown.empty() || shown.size() > kMaxGroupName) return std::nullopt;
    Group g;
    g.name = shown;
    randombytes_buf(g.secret.data(), g.secret.size());
    return g;
}

std::string Group::invite() const {
    std::string raw(1, static_cast<char>(kInviteVersion));
    raw.append(reinterpret_cast<const char*>(secret.data()), secret.size());
    raw += name;
    const std::string out = kInvitePrefix + b64(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
                                                sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_memzero(raw.data(), raw.size());
    return out;
}

std::optional<Group> Group::from_invite(const std::string& text) {
    // Pasted text: surrounding whitespace is forgiven, nothing else.
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    const std::string s = text.substr(a, b - a);
    const std::string prefix = kInvitePrefix;
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    auto raw = unb64(s.substr(prefix.size()), sodium_base64_VARIANT_URLSAFE_NO_PADDING, 512);
    if (!raw || raw->size() < 1 + 32 + 1 || static_cast<uint8_t>((*raw)[0]) != kInviteVersion) return std::nullopt;
    const std::string name = raw->substr(33);
    if (name.size() > kMaxGroupName) return std::nullopt;
    Group g;
    g.name = display_alias(name);
    if (g.name.empty()) return std::nullopt;
    std::memcpy(g.secret.data(), raw->data() + 1, 32);
    sodium_memzero(raw->data(), raw->size());
    return g;
}

std::string Group::forum_id() const {
    const auto k = subkey(secret, kForumKey);
    return "g:" + to_hex(k.data(), 16);
}

std::string Group::content_topic() const {
    // The forum's own app and version, so a group shares the public forum's
    // shard (its traffic mixes with the forum's on the relays) while staying
    // a topic no public forum node subscribes to.
    const auto k = subkey(secret, kTopicKey);
    return "/logos-forum/1/g-" + to_hex(k.data(), 16) + "/sealed";
}

SealKey Group::seal_key() const { return subkey(secret, kSealKey); }

std::string seal(const SealKey& key, const std::string& topic, const std::string& plain) {
    // Padded: a 4-byte length, the message, zeros to the next KiB.
    std::string padded(4, '\0');
    const uint32_t n = static_cast<uint32_t>(plain.size());
    for (int i = 0; i < 4; ++i) padded[i] = static_cast<char>(n >> (8 * i));
    padded += plain;
    padded.resize((padded.size() + kPad - 1) / kPad * kPad, '\0');
    const std::string ad = kAdDomain + topic;
    std::string out(1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + padded.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    auto* o = reinterpret_cast<unsigned char*>(out.data());
    o[0] = kSealVersion;
    randombytes_buf(o + 1, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(o + 1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &clen,
                                               reinterpret_cast<const unsigned char*>(padded.data()), padded.size(),
                                               reinterpret_cast<const unsigned char*>(ad.data()), ad.size(), nullptr,
                                               o + 1, key.data());
    sodium_memzero(padded.data(), padded.size());
    return b64(o, out.size(), sodium_base64_VARIANT_ORIGINAL);
}

std::optional<std::string> open_sealed(const SealKey& key, const std::string& topic, const std::string& wire) {
    if (wire.size() > kMaxSealedBytes) return std::nullopt;
    auto raw = unb64(wire, sodium_base64_VARIANT_ORIGINAL, kMaxSealedBytes);
    constexpr size_t head = 1 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (!raw || raw->size() < head + crypto_aead_xchacha20poly1305_ietf_ABYTES + 4 ||
        static_cast<uint8_t>((*raw)[0]) != kSealVersion)
        return std::nullopt;
    const auto* r = reinterpret_cast<const unsigned char*>(raw->data());
    const std::string ad = kAdDomain + topic;
    std::string padded(raw->size() - head - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long plen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(padded.data()), &plen, nullptr, r + head,
                                                   raw->size() - head, reinterpret_cast<const unsigned char*>(ad.data()),
                                                   ad.size(), r + 1, key.data()) != 0)
        return std::nullopt;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<unsigned char>(padded[i])) << (8 * i);
    if (n > plen - 4) return std::nullopt;
    return padded.substr(4, n);
}

SealedTransport::~SealedTransport() { sodium_memzero(key_.data(), key_.size()); }

SendResult SealedTransport::send(const std::string&, const std::string& payload) {
    return inner_.send(topic_, seal(key_, topic_, payload));
}

bool SealedTransport::subscribe(const std::string&) { return inner_.subscribe(topic_); }

std::vector<std::string> SealedTransport::history(const std::string&) {
    std::vector<std::string> out;
    for (const auto& w : inner_.history(topic_))
        if (auto p = open(w)) out.push_back(std::move(*p));
    return out;
}

// ─── Joined groups, in forum.db ─────────────────────────────────────────────

Groups::Groups(Store& store, const Keystore* keys) : store_(store), keys_(keys) {
    sqlite3_exec(store_.handle(),
                 "CREATE TABLE IF NOT EXISTS forum_groups (forum TEXT PRIMARY KEY, name TEXT NOT NULL, secret BLOB NOT NULL,"
                 " joined INTEGER NOT NULL)",
                 nullptr, nullptr, nullptr);
}

bool Groups::save(const Group& g, uint64_t now_ms) {
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(store_.handle(), "INSERT OR IGNORE INTO forum_groups VALUES (?,?,?,?)", -1, &q, nullptr) != SQLITE_OK) return false;
    const std::string id = g.forum_id();
    SecretBytes stored(g.secret.begin(), g.secret.end());
    if (keys_ && !keys_->seal_secret(id, g.secret.data(), g.secret.size(), stored)) {
        sqlite3_finalize(q);
        return false;   // sealed keys, locked: not written in plain text
    }
    sqlite3_bind_text(q, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, g.name.data(), static_cast<int>(g.name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(q, 3, stored.data(), static_cast<int>(stored.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 4, static_cast<int64_t>(now_ms));
    const bool ok = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(store_.handle()) == 1;
    sqlite3_finalize(q);
    return ok;
}

std::vector<Group> Groups::all() const {
    std::vector<Group> out;
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(store_.handle(), "SELECT name, secret, forum FROM forum_groups ORDER BY joined, forum", -1, &q, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(q) == SQLITE_ROW) {
        Group g;
        const auto* t = sqlite3_column_text(q, 0);
        g.name = t ? reinterpret_cast<const char*>(t) : "";
        const auto* b = static_cast<const uint8_t*>(sqlite3_column_blob(q, 1));
        SecretBytes stored;
        if (b) stored.assign(b, b + sqlite3_column_bytes(q, 1));
        const auto* f = sqlite3_column_text(q, 2);
        const std::string forum = f ? reinterpret_cast<const char*>(f) : "";
        // Sealed and locked (or damaged): that group stays closed.
        if (keys_) {
            if (!keys_->open_secret(forum, stored, g.secret.data(), g.secret.size())) continue;
        } else {
            if (stored.size() != g.secret.size()) continue;
            std::memcpy(g.secret.data(), stored.data(), g.secret.size());
        }
        if (g.forum_id() != forum) continue;   // a secret moved to another row
        out.push_back(std::move(g));
    }
    sqlite3_finalize(q);
    return out;
}

bool Groups::remove(const std::string& forum_id) {
    sqlite3* db = store_.handle();
    bool removed = false;
    for (const char* sql : {"DELETE FROM outbox WHERE id IN (SELECT id FROM posts WHERE forum=?1)",
                            "DELETE FROM posts WHERE forum=?1", "DELETE FROM forum_groups WHERE forum=?1"}) {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &q, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_text(q, 1, forum_id.data(), static_cast<int>(forum_id.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_DONE && std::strstr(sql, "forum_groups") && sqlite3_changes(db) > 0) removed = true;
        sqlite3_finalize(q);
    }
    store_.scrub();
    return removed;
}

} // namespace forum
