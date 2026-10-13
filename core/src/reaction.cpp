// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/reaction.h"

#include "forum/store.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstring>

namespace forum {

const char* const kEmoji[kEmojiCount] = {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x8E\x89",
                                         "\xF0\x9F\x91\x80", "\xE2\x9C\x85"};

namespace {

using json = nlohmann::json;
constexpr const char* kTag = "logos-forum-reaction";

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_str(std::vector<uint8_t>& out, const std::string& s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}
std::string b64(const uint8_t* d, size_t n) {
    std::string out(sodium_base64_encoded_len(n, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), d, n, sodium_base64_VARIANT_ORIGINAL);
    out.resize(out.find('\0'));
    return out;
}
template <size_t N> bool unb64(const std::string& s, std::array<uint8_t, N>& out) {
    if (s.size() > (N + 2) / 3 * 4) return false;
    size_t len = 0;
    if (sodium_base642bin(out.data(), N, s.data(), s.size(), nullptr, &len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        return false;
    return len == N;
}
bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS reactions (
  author BLOB NOT NULL, target TEXT NOT NULL, emoji INTEGER NOT NULL, forum TEXT NOT NULL,
  on_ INTEGER NOT NULL, ts INTEGER NOT NULL, sig BLOB NOT NULL,
  PRIMARY KEY (author, target, emoji));
CREATE INDEX IF NOT EXISTS reactions_target ON reactions(target);
)sql";

Reaction row(sqlite3_stmt* s) {
    Reaction r;
    if (sqlite3_column_bytes(s, 0) == 32) std::memcpy(r.author.data(), sqlite3_column_blob(s, 0), 32);
    const auto* t = sqlite3_column_text(s, 1);
    if (t) r.target.assign(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, 1));
    r.emoji = sqlite3_column_int(s, 2);
    const auto* f = sqlite3_column_text(s, 3);
    if (f) r.forum.assign(reinterpret_cast<const char*>(f), sqlite3_column_bytes(s, 3));
    r.on = sqlite3_column_int(s, 4) != 0;
    r.ts_ms = static_cast<uint64_t>(sqlite3_column_int64(s, 5));
    if (sqlite3_column_bytes(s, 6) == 64) std::memcpy(r.sig.data(), sqlite3_column_blob(s, 6), 64);
    return r;
}

} // namespace

std::vector<uint8_t> Reaction::signing_bytes() const {
    std::vector<uint8_t> out;
    put_str(out, "logos-forum:reaction:v1");
    put_str(out, forum);
    put_str(out, target);
    put_u32(out, static_cast<uint32_t>(emoji));
    out.push_back(on ? 1 : 0);
    put_u64(out, ts_ms);
    out.insert(out.end(), author.begin(), author.end());
    return out;
}

void sign(Reaction& r, const SecretKey& sk) {
    const auto bytes = r.signing_bytes();
    crypto_sign_detached(r.sig.data(), nullptr, bytes.data(), bytes.size(), sk.data());
}

bool well_formed(const Reaction& r) {
    return !r.forum.empty() && r.forum.size() <= kMaxForum && is_hex64(r.target) && r.emoji >= 0 && r.emoji < kEmojiCount;
}

bool verify(const Reaction& r) {
    if (!well_formed(r)) return false;
    const auto bytes = r.signing_bytes();
    return crypto_sign_verify_detached(r.sig.data(), bytes.data(), bytes.size(), r.author.data()) == 0;
}

std::string encode(const Reaction& r) {
    return json{{kTag, 1},
                {"forum", r.forum},
                {"target", r.target},
                {"emoji", r.emoji},
                {"on", r.on},
                {"ts", r.ts_ms},
                {"author", b64(r.author.data(), r.author.size())},
                {"sig", b64(r.sig.data(), r.sig.size())}}
        .dump();
}

namespace {
std::optional<Reaction> from_json(const json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto tag = j.find(kTag);
    if (tag == j.end() || !tag->is_number_integer() || tag->get<int64_t>() != 1) return std::nullopt;
    const auto f = j.find("forum"), t = j.find("target"), e = j.find("emoji"), o = j.find("on"), ts = j.find("ts"),
               a = j.find("author"), s = j.find("sig");
    if (f == j.end() || !f->is_string() || t == j.end() || !t->is_string() || e == j.end() || !e->is_number_integer() ||
        o == j.end() || !o->is_boolean() || ts == j.end() || !ts->is_number_unsigned() || a == j.end() || !a->is_string() ||
        s == j.end() || !s->is_string())
        return std::nullopt;
    Reaction r;
    r.forum = f->get<std::string>();
    r.target = t->get<std::string>();
    const int64_t emoji = e->get<int64_t>();
    if (emoji < 0 || emoji >= kEmojiCount) return std::nullopt;
    r.emoji = static_cast<int>(emoji);
    r.on = o->get<bool>();
    r.ts_ms = ts->get<uint64_t>();
    if (!unb64(a->get<std::string>(), r.author) || !unb64(s->get<std::string>(), r.sig)) return std::nullopt;
    if (!well_formed(r)) return std::nullopt;
    return r;
}
} // namespace

std::optional<Reaction> decode_reaction(const std::string& text) {
    return from_json(json::parse(text, nullptr, false));
}

Reactions::Reactions(Store& store) : db_(store.handle()), store_(store) {
    sqlite3_exec(db_, kSchema, nullptr, nullptr, nullptr);
}

bool Reactions::put(const Reaction& r) {
    // The newest signed state wins; at the same date, "off" wins, so every
    // peer settles on the same state whatever the order of arrival.
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO reactions VALUES (?,?,?,?,?,?,?) ON CONFLICT(author,target,emoji) DO UPDATE SET "
                           "on_=excluded.on_, ts=excluded.ts, sig=excluded.sig, forum=excluded.forum "
                           "WHERE excluded.ts > reactions.ts OR (excluded.ts = reactions.ts AND excluded.on_ < reactions.on_)",
                           -1, &s, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, r.author.data(), 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, r.target.data(), static_cast<int>(r.target.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, r.emoji);
    sqlite3_bind_text(s, 4, r.forum.data(), static_cast<int>(r.forum.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, r.on ? 1 : 0);
    sqlite3_bind_int64(s, 6, static_cast<int64_t>(r.ts_ms));
    sqlite3_bind_blob(s, 7, r.sig.data(), 64, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    sqlite3_finalize(s);
    return ok;
}

Reactions::Outcome Reactions::receive(const Reaction& r, uint64_t now_ms, Origin from) {
    const bool live = from == Origin::Live;
    auto drop = [&](Outcome o) { ++dropped_; return o; };
    if (!well_formed(r)) return drop(Outcome::Malformed);
    // A key that has spent its allowance (with reactions it really signed):
    // dropped before anything else. Only read here, so a forger naming
    // someone's key cannot spend it.
    if (live) {
        const auto k = keys_.find(r.author);
        if (k != keys_.end() && !k->second.bucket.ready(now_ms)) return drop(Outcome::OverKeyLimit);
    }
    // Before the signature: anyone can send these for free, so the whole
    // topic gets a budget of checks, on arrival time.
    if (live && !topic_.take(now_ms)) return drop(Outcome::OverBudget);
    if (!live && !history_.take(now_ms)) return drop(Outcome::OverBudget);
    ++verified_;
    if (!verify(r)) return drop(Outcome::BadSignature);
    if (live) {
        // Per key, once the key is known to have signed it (a forger cannot
        // spend someone else's allowance).
        auto it = keys_.find(r.author);
        if (it == keys_.end()) {
            if (keys_.size() >= kMaxTrackedKeys) {
                auto oldest = std::min_element(keys_.begin(), keys_.end(),
                                               [](const auto& a, const auto& b) { return a.second.last_ms < b.second.last_ms; });
                keys_.erase(oldest);
            }
            it = keys_.emplace(r.author, KeyBucket{}).first;
        }
        it->second.last_ms = now_ms;
        if (!it->second.bucket.take(now_ms)) return drop(Outcome::OverKeyLimit);
    }
    // Only on posts we hold, in the reaction's own forum: storage stays
    // bounded by real posts, and a public reaction cannot count on a post of
    // a private group (or the other way round).
    const auto target = store_.get(r.target);
    if (!target || target->forum != r.forum) return drop(Outcome::UnknownPost);
    return put(r) ? Outcome::Stored : Outcome::Unchanged;
}

std::optional<Reaction> Reactions::get(const PublicKey& author, const std::string& target, int emoji) const {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT author,target,emoji,forum,on_,ts,sig FROM reactions WHERE author=? AND target=? AND emoji=?",
                           -1, &s, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_blob(s, 1, author.data(), 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, target.data(), static_cast<int>(target.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, emoji);
    std::optional<Reaction> out;
    if (sqlite3_step(s) == SQLITE_ROW) out = row(s);
    sqlite3_finalize(s);
    return out;
}

std::array<int, kEmojiCount> Reactions::counts(const std::string& target) const {
    std::array<int, kEmojiCount> out{};
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT emoji, COUNT(*) FROM reactions WHERE target=? AND on_=1 GROUP BY emoji", -1, &s,
                           nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(s, 1, target.data(), static_cast<int>(target.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const int e = sqlite3_column_int(s, 0);
        if (e >= 0 && e < kEmojiCount) out[e] = sqlite3_column_int(s, 1);
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<Reaction> Reactions::on_posts(const std::vector<std::string>& ids, size_t limit) const {
    std::vector<Reaction> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT author,target,emoji,forum,on_,ts,sig FROM reactions WHERE target=? ORDER BY ts DESC",
                           -1, &s, nullptr) != SQLITE_OK)
        return out;
    for (const auto& id : ids) {
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
        while (out.size() < limit && sqlite3_step(s) == SQLITE_ROW) out.push_back(row(s));
        if (out.size() >= limit) break;
    }
    sqlite3_finalize(s);
    return out;
}

} // namespace forum
