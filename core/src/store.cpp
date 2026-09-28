// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/store.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace forum {
namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& text(int i, const std::string& v) {
        sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& blob(int i, const void* d, size_t n) {
        sqlite3_bind_blob(s, i, d, static_cast<int>(n), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& i64(int i, int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    bool step() {
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
    }
    std::string str(int c) const {
        const auto* t = sqlite3_column_text(s, c);
        return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(s, c)) : std::string();
    }
    int64_t num(int c) const { return sqlite3_column_int64(s, c); }
    template <size_t N> void arr(int c, std::array<uint8_t, N>& out) const {
        const void* b = sqlite3_column_blob(s, c);
        if (b && sqlite3_column_bytes(s, c) == static_cast<int>(N)) std::memcpy(out.data(), b, N);
    }
};

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS posts (
  id TEXT PRIMARY KEY, forum TEXT NOT NULL, kind INTEGER NOT NULL, topic_id TEXT NOT NULL,
  title TEXT NOT NULL, body TEXT NOT NULL, mode INTEGER NOT NULL, alias TEXT NOT NULL,
  ts INTEGER NOT NULL, author BLOB NOT NULL, sig BLOB NOT NULL, received INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS posts_topic ON posts(topic_id);
CREATE INDEX IF NOT EXISTS posts_forum ON posts(forum, kind);
CREATE TABLE IF NOT EXISTS outbox (
  id TEXT PRIMARY KEY, payload TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
  next_try INTEGER NOT NULL, last_error TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS accounts (
  label TEXT PRIMARY KEY, pk BLOB NOT NULL, sk BLOB NOT NULL, created INTEGER NOT NULL,
  posts INTEGER NOT NULL, selected INTEGER NOT NULL DEFAULT 0);
)sql";

Post row_to_post(const Stmt& q) {
    Post p;
    p.forum = q.str(1);
    p.kind = q.num(2) == 0 ? Kind::Topic : Kind::Reply;
    p.topic_id = q.str(3);
    p.title = q.str(4);
    p.body = q.str(5);
    p.mode = static_cast<Mode>(q.num(6));
    p.alias = q.str(7);
    p.ts_ms = static_cast<uint64_t>(q.num(8));
    q.arr(9, p.author);
    q.arr(10, p.sig);
    return p;
}

constexpr const char* kCols = "id,forum,kind,topic_id,title,body,mode,alias,ts,author,sig";

} // namespace

uint64_t backoff_ms(int attempts) {
    const int n = std::clamp(attempts, 1, 20);
    const uint64_t ms = 1000ull << n;
    return std::min<uint64_t>(ms, 5 * 60 * 1000);
}

Store::Store(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        throw std::runtime_error(std::string("cannot open store: ") + path);
    exec("PRAGMA journal_mode=WAL;");
    exec(kSchema);
}

Store::~Store() { sqlite3_close(db_); }

void Store::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "?";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + e);
    }
}

bool Store::put(const Post& p, uint64_t received_ms) {
    Stmt q(db_, "INSERT OR IGNORE INTO posts VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    q.text(1, p.id()).text(2, p.forum).i64(3, p.kind == Kind::Topic ? 0 : 1).text(4, p.topic_id)
        .text(5, p.title).text(6, p.body).i64(7, static_cast<int>(p.mode)).text(8, p.alias)
        .i64(9, static_cast<int64_t>(p.ts_ms)).blob(10, p.author.data(), 32).blob(11, p.sig.data(), 64)
        .i64(12, static_cast<int64_t>(received_ms));
    q.step();
    return sqlite3_changes(db_) == 1;
}

bool Store::has(const std::string& id) const {
    Stmt q(db_, "SELECT 1 FROM posts WHERE id=?");
    q.text(1, id);
    return q.step();
}

std::optional<Post> Store::get(const std::string& id) const {
    Stmt q(db_, (std::string("SELECT ") + kCols + " FROM posts WHERE id=?").c_str());
    q.text(1, id);
    if (!q.step()) return std::nullopt;
    return row_to_post(q);
}

std::vector<TopicRow> Store::topics(const std::string& forum) const {
    Stmt q(db_, (std::string("SELECT ") + kCols + ",(SELECT COUNT(*) FROM posts r WHERE r.topic_id=t.id),"
                 "MAX(t.ts,COALESCE((SELECT MAX(ts) FROM posts r WHERE r.topic_id=t.id),0)) AS act "
                 "FROM posts t WHERE forum=? AND kind=0 ORDER BY act DESC").c_str());
    q.text(1, forum);
    std::vector<TopicRow> out;
    while (q.step()) {
        TopicRow r;
        r.topic = row_to_post(q);
        r.id = q.str(0);
        r.replies = static_cast<int>(q.num(11));
        r.last_activity_ms = static_cast<uint64_t>(q.num(12));
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Post> Store::replies(const std::string& topic_id) const {
    Stmt q(db_, (std::string("SELECT ") + kCols + " FROM posts WHERE topic_id=? AND kind=1 ORDER BY ts, id").c_str());
    q.text(1, topic_id);
    std::vector<Post> out;
    while (q.step()) out.push_back(row_to_post(q));
    return out;
}

size_t Store::count() const {
    Stmt q(db_, "SELECT COUNT(*) FROM posts");
    q.step();
    return static_cast<size_t>(q.num(0));
}

void Store::enqueue(const std::string& id, const std::string& payload, uint64_t now_ms) {
    Stmt q(db_, "INSERT OR IGNORE INTO outbox(id,payload,next_try) VALUES (?,?,?)");
    q.text(1, id).text(2, payload).i64(3, static_cast<int64_t>(now_ms));
    q.step();
}

static std::vector<OutboxItem> read_outbox(sqlite3* db, const char* sql, const int64_t* now) {
    Stmt q(db, sql);
    if (now) q.i64(1, *now);
    std::vector<OutboxItem> out;
    while (q.step())
        out.push_back({q.str(0), q.str(1), static_cast<int>(q.num(2)), static_cast<uint64_t>(q.num(3)), q.str(4)});
    return out;
}

std::vector<OutboxItem> Store::due(uint64_t now_ms) const {
    const int64_t now = static_cast<int64_t>(now_ms);
    return read_outbox(db_, "SELECT id,payload,attempts,next_try,last_error FROM outbox WHERE next_try<=? ORDER BY next_try", &now);
}

std::vector<OutboxItem> Store::outbox() const {
    return read_outbox(db_, "SELECT id,payload,attempts,next_try,last_error FROM outbox ORDER BY next_try", nullptr);
}

void Store::sent(const std::string& id) {
    Stmt q(db_, "DELETE FROM outbox WHERE id=?");
    q.text(1, id);
    q.step();
}

void Store::failed(const std::string& id, const std::string& error, uint64_t now_ms) {
    int attempts = 0;
    {
        Stmt q(db_, "SELECT attempts FROM outbox WHERE id=?");
        q.text(1, id);
        if (!q.step()) return;
        attempts = static_cast<int>(q.num(0)) + 1;
    }
    Stmt u(db_, "UPDATE outbox SET attempts=?, next_try=?, last_error=? WHERE id=?");
    u.i64(1, attempts).i64(2, static_cast<int64_t>(now_ms + backoff_ms(attempts))).text(3, error).text(4, id);
    u.step();
}

void Store::save_account(const Account& a, bool selected) {
    if (selected) exec("UPDATE accounts SET selected=0");
    Stmt q(db_, "INSERT INTO accounts VALUES (?,?,?,?,?,?) ON CONFLICT(label) DO UPDATE SET "
                "pk=excluded.pk, sk=excluded.sk, created=excluded.created, posts=excluded.posts, "
                "selected=MAX(selected, excluded.selected)");
    q.text(1, a.label).blob(2, a.key.pk.data(), 32).blob(3, a.key.sk.data(), 64)
        .i64(4, static_cast<int64_t>(a.created_ms)).i64(5, a.posts).i64(6, selected ? 1 : 0);
    q.step();
}

std::vector<Account> Store::accounts() const {
    Stmt q(db_, "SELECT label,pk,sk,created,posts FROM accounts ORDER BY created, label");
    std::vector<Account> out;
    while (q.step()) {
        Account a;
        a.label = q.str(0);
        q.arr(1, a.key.pk);
        q.arr(2, a.key.sk);
        a.created_ms = static_cast<uint64_t>(q.num(3));
        a.posts = static_cast<uint32_t>(q.num(4));
        out.push_back(std::move(a));
    }
    return out;
}

std::optional<std::string> Store::selected_label() const {
    Stmt q(db_, "SELECT label FROM accounts WHERE selected=1");
    if (!q.step()) return std::nullopt;
    return q.str(0);
}

void Store::remove_account(const std::string& label) {
    Stmt q(db_, "DELETE FROM accounts WHERE label=?");
    q.text(1, label);
    q.step();
}

} // namespace forum
