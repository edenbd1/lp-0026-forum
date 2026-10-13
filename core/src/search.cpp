// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/search.h"

#include "forum/engine.h"
#include "forum/store.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace forum {
namespace {

// The body as indexed: without the image line (see media.h), which is ASCII
// and 112 characters long, so plain SQL can cut it.
constexpr const char* kIndexedBody =
    "CASE WHEN substr(%s.body, -112, 28) = char(10,10) || '[Images on Logos Storage: ' "
    "THEN substr(%s.body, 1, length(%s.body) - 112) ELSE %s.body END";

std::string body_expr(const char* row) {
    char buf[512];
    std::snprintf(buf, sizeof buf, kIndexedBody, row, row, row, row);
    return buf;
}

std::vector<std::string> words(const std::string& typed) {
    std::string s = typed.substr(0, utf8_cut(typed, 0, kMaxSearchBytes));
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7F) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (out.size() >= kMaxSearchWords) break;
        } else {
            cur.push_back(static_cast<char>(c));
        }
    }
    if (!cur.empty() && out.size() < kMaxSearchWords) out.push_back(cur);
    return out;
}

std::string col(sqlite3_stmt* s, int c) {
    const auto* t = sqlite3_column_text(s, c);
    return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, c)) : std::string();
}

} // namespace

std::string fts_query(const std::string& typed) {
    std::string q;
    const auto w = words(typed);
    for (size_t i = 0; i < w.size(); ++i) {
        if (!q.empty()) q += ' ';
        q += '"';
        for (char c : w[i]) {
            if (c == '"') q += '"';
            q += c;
        }
        q += '"';
        if (i + 1 == w.size()) q += '*';  // the word being typed: a prefix
    }
    return q;
}

Search::Search(Store& store, bool use_fts) : db_(store.handle()) {
    if (!use_fts) return;
    const std::string body = body_expr("new");
    const std::string sql =
        "CREATE VIRTUAL TABLE IF NOT EXISTS posts_fts USING fts5(title, body, tokenize='unicode61 remove_diacritics 2');"
        "CREATE TRIGGER IF NOT EXISTS posts_fts_ai AFTER INSERT ON posts BEGIN "
        "  INSERT INTO posts_fts(rowid, title, body) VALUES (new.rowid, new.title, " + body + "); END;"
        "CREATE TRIGGER IF NOT EXISTS posts_fts_ad AFTER DELETE ON posts BEGIN "
        "  DELETE FROM posts_fts WHERE rowid = old.rowid; END;";
    fts_ = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!fts_) return;
    // Posts stored before the index existed (or by a version without it).
    const std::string fill = "INSERT INTO posts_fts(rowid, title, body) SELECT p.rowid, p.title, " + body_expr("p") +
                             " FROM posts p WHERE p.rowid NOT IN (SELECT rowid FROM posts_fts)";
    sqlite3_exec(db_, fill.c_str(), nullptr, nullptr, nullptr);
}

std::vector<SearchHit> Search::find(const std::string& forum, const std::string& typed, size_t limit,
                                    const std::function<bool(const SearchHit&)>& hidden) const {
    std::vector<SearchHit> out;
    const auto w = words(typed);
    if (w.empty() || limit == 0) return out;
    // Read more than asked when some may be hidden, up to a bound.
    const int64_t want = static_cast<int64_t>(std::min<size_t>(hidden ? limit * 4 : limit, 1000));
    sqlite3_stmt* s = nullptr;
    std::string sql;
    if (fts_) {
        sql = "SELECT p.id, p.kind, p.topic_id, p.title, snippet(posts_fts, -1, '', '', '…', 16), p.author, p.ts "
              "FROM posts_fts JOIN posts p ON p.rowid = posts_fts.rowid "
              "WHERE posts_fts MATCH ? AND p.forum = ? ORDER BY bm25(posts_fts) LIMIT ?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return out;
        const std::string q = fts_query(typed);
        sqlite3_bind_text(s, 1, q.data(), static_cast<int>(q.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, forum.data(), static_cast<int>(forum.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, want);
    } else {
        // Every word, in the title or the body; LIKE wildcards in what was
        // typed are escaped, and every value is a bound parameter.
        const std::string body = body_expr("p");
        sql = "SELECT p.id, p.kind, p.topic_id, p.title, " + body + ", p.author, p.ts FROM posts p WHERE p.forum = ?";
        for (size_t i = 0; i < w.size(); ++i)
            sql += " AND (p.title LIKE ? ESCAPE '\\' OR " + body + " LIKE ? ESCAPE '\\')";
        sql += " ORDER BY p.ts DESC LIMIT ?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return out;
        int k = 1;
        sqlite3_bind_text(s, k++, forum.data(), static_cast<int>(forum.size()), SQLITE_TRANSIENT);
        for (const auto& word : w) {
            std::string pat = "%";
            for (char c : word) {
                if (c == '%' || c == '_' || c == '\\') pat += '\\';
                pat += c;
            }
            pat += '%';
            sqlite3_bind_text(s, k++, pat.data(), static_cast<int>(pat.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(s, k++, pat.data(), static_cast<int>(pat.size()), SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(s, k, want);
    }
    while (out.size() < limit && sqlite3_step(s) == SQLITE_ROW) {
        SearchHit h;
        h.id = col(s, 0);
        h.kind = sqlite3_column_int(s, 1) == 0 ? Kind::Topic : Kind::Reply;
        h.topic_id = h.kind == Kind::Topic ? h.id : col(s, 2);
        h.title = col(s, 3);
        h.snippet = col(s, 4);
        if (sqlite3_column_bytes(s, 5) == 32) std::memcpy(h.author.data(), sqlite3_column_blob(s, 5), 32);
        h.ts_ms = static_cast<uint64_t>(sqlite3_column_int64(s, 6));
        if (hidden && hidden(h)) continue;
        if (!fts_ && h.snippet.size() > 200) h.snippet = h.snippet.substr(0, utf8_cut(h.snippet, 0, 200)) + "…";
        out.push_back(std::move(h));
    }
    sqlite3_finalize(s);
    return out;
}

} // namespace forum
