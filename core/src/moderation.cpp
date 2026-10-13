// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/moderation.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace forum {
namespace {

// Never throws, like the store: a failed statement reads as no rows.
struct Q {
    sqlite3* db;
    sqlite3_stmt* s = nullptr;
    Q(sqlite3* d, const char* sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Q() { sqlite3_finalize(s); }
    Q& text(int i, const std::string& v) {
        if (s) sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Q& blob(int i, const PublicKey& k) {
        if (s) sqlite3_bind_blob(s, i, k.data(), static_cast<int>(k.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Q& i64(int i, int64_t v) {
        if (s) sqlite3_bind_int64(s, i, v);
        return *this;
    }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool changed() { return s && sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) > 0; }
};

bool is_post_id(const std::string& id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

Moderation::Moderation(Store& store) : store_(store) {
    sqlite3_exec(store_.handle(),
                 "CREATE TABLE IF NOT EXISTS muted (account TEXT NOT NULL, author BLOB NOT NULL, at INTEGER NOT NULL,"
                 " PRIMARY KEY (account, author));"
                 "CREATE TABLE IF NOT EXISTS hidden (account TEXT NOT NULL, post TEXT NOT NULL, at INTEGER NOT NULL,"
                 " PRIMARY KEY (account, post));",
                 nullptr, nullptr, nullptr);
}

bool Moderation::mute(const std::string& account, const PublicKey& key, uint64_t now_ms) {
    return Q(store_.handle(), "INSERT OR IGNORE INTO muted VALUES (?,?,?)").text(1, account).blob(2, key).i64(3, static_cast<int64_t>(now_ms)).changed();
}

bool Moderation::unmute(const std::string& account, const PublicKey& key) {
    return Q(store_.handle(), "DELETE FROM muted WHERE account=? AND author=?").text(1, account).blob(2, key).changed();
}

bool Moderation::is_muted(const std::string& account, const PublicKey& key) const {
    return Q(store_.handle(), "SELECT 1 FROM muted WHERE account=? AND author=?").text(1, account).blob(2, key).row();
}

std::vector<Muted> Moderation::muted(const std::string& account) const {
    Q q(store_.handle(), "SELECT author, at FROM muted WHERE account=? ORDER BY at DESC");
    q.text(1, account);
    std::vector<Muted> out;
    while (q.row()) {
        Muted m;
        if (sqlite3_column_bytes(q.s, 0) != 32) continue;
        std::memcpy(m.key.data(), sqlite3_column_blob(q.s, 0), 32);
        m.at_ms = static_cast<uint64_t>(sqlite3_column_int64(q.s, 1));
        out.push_back(m);
    }
    return out;
}

bool Moderation::hide(const std::string& account, const std::string& id, uint64_t now_ms) {
    if (!is_post_id(id)) return false;
    return Q(store_.handle(), "INSERT OR IGNORE INTO hidden VALUES (?,?,?)").text(1, account).text(2, id).i64(3, static_cast<int64_t>(now_ms)).changed();
}

bool Moderation::unhide(const std::string& account, const std::string& id) {
    return Q(store_.handle(), "DELETE FROM hidden WHERE account=? AND post=?").text(1, account).text(2, id).changed();
}

bool Moderation::is_hidden(const std::string& account, const std::string& id) const {
    return Q(store_.handle(), "SELECT 1 FROM hidden WHERE account=? AND post=?").text(1, account).text(2, id).row();
}

std::vector<Hidden> Moderation::hidden(const std::string& account) const {
    Q q(store_.handle(), "SELECT post, at FROM hidden WHERE account=? ORDER BY at DESC");
    q.text(1, account);
    std::vector<Hidden> out;
    while (q.row()) {
        const auto* t = sqlite3_column_text(q.s, 0);
        out.push_back({t ? reinterpret_cast<const char*>(t) : "", static_cast<uint64_t>(sqlite3_column_int64(q.s, 1))});
    }
    return out;
}

std::vector<TopicRow> Moderation::visible(const std::string& account, std::vector<TopicRow> topics) const {
    std::set<PublicKey> keys;
    for (const auto& m : muted(account)) keys.insert(m.key);
    std::set<std::string> ids;
    for (const auto& h : hidden(account)) ids.insert(h.id);
    topics.erase(std::remove_if(topics.begin(), topics.end(),
                                [&](const TopicRow& t) { return keys.count(t.topic.author) || ids.count(t.id); }),
                 topics.end());
    return topics;
}

std::vector<Post> Moderation::visible(const std::string& account, std::vector<Post> replies) const {
    std::set<PublicKey> keys;
    for (const auto& m : muted(account)) keys.insert(m.key);
    std::set<std::string> ids;
    for (const auto& h : hidden(account)) ids.insert(h.id);
    replies.erase(std::remove_if(replies.begin(), replies.end(),
                                 [&](const Post& p) { return keys.count(p.author) || ids.count(p.id()); }),
                  replies.end());
    return replies;
}

void Moderation::forget(const std::string& account) {
    Q(store_.handle(), "DELETE FROM muted WHERE account=?").text(1, account).changed();
    Q(store_.handle(), "DELETE FROM hidden WHERE account=?").text(1, account).changed();
}

} // namespace forum
