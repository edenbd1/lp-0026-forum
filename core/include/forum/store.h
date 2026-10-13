// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Everything the forum keeps on this machine, in one SQLite file.
//
// Posts are keyed by their content id, so the same post arriving live, from a
// store query and from a history snapshot is stored once. The outbox holds what
// the user wrote until the network has taken it: a post composed offline, or
// whose send failed, stays here and is retried — it is never lost because the
// node was down when "Post" was pressed.
#pragma once

#include "forum/identity.h"
#include "forum/post.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace forum {

struct TopicRow {
    Post topic;
    std::string id;
    int replies = 0;
    uint64_t last_activity_ms = 0;
};

// A post with the time this install stored it.
struct Stored {
    Post post;
    std::string id;
    uint64_t received_ms = 0;
};

// Bytes that held a secret: zeroed before the memory is given back, also when
// a vector grows and moves its contents.
template <class T> struct WipingAllocator {
    using value_type = T;
    WipingAllocator() = default;
    template <class U> WipingAllocator(const WipingAllocator<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    void deallocate(T* p, size_t n);
    template <class U> bool operator==(const WipingAllocator<U>&) const { return true; }
};
using SecretBytes = std::vector<uint8_t, WipingAllocator<uint8_t>>;
void wipe_bytes(void* p, size_t n);
template <class T> void WipingAllocator<T>::deallocate(T* p, size_t n) {
    wipe_bytes(p, n * sizeof(T));
    ::operator delete(p);
}

struct OutboxItem {
    std::string id;
    std::string payload;  // the encoded post
    int attempts = 0;
    uint64_t next_try_ms = 0;
    std::string last_error;
};

class Store {
public:
    // ":memory:" for tests.
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // True if the post was new; false if its id was already stored.
    bool put(const Post& p, uint64_t received_ms);
    bool has(const std::string& id) const;
    // Whether the store holds a post of `forum` signed by `author`.
    bool has_author(const std::string& forum, const PublicKey& author) const;
    std::optional<Post> get(const std::string& id) const;

    // Topics of a forum, most recently active first.
    std::vector<TopicRow> topics(const std::string& forum) const;
    // Replies to a topic, oldest first.
    std::vector<Post> replies(const std::string& topic_id) const;
    size_t count() const;
    // How many posts a forum holds, without reading them.
    size_t count(const std::string& forum) const;
    // The newest post date in a forum no later than `limit_ms` (0 if none), so
    // a post dated in the future cannot stand in for "the newest".
    // Only posts received before `received_before_ms` count, if given.
    uint64_t newest_ts(const std::string& forum, uint64_t limit_ms, uint64_t received_before_ms = UINT64_MAX >> 1) const;
    // Every post of a forum, oldest first.
    std::vector<Post> all(const std::string& forum) const;
    // Posts in the order history is paged (newest first, ties by id), dated
    // from `since_ms`, strictly after the cursor (`before_ts`, `after_id`): a
    // post is after it if older, or as old with a greater id. The cursor
    // (`before_ts`, "") starts with everything dated before `before_ts`.
    // At most `limit`, read without loading the rest.
    std::vector<Post> recent(const std::string& forum, uint64_t since_ms, uint64_t before_ts,
                             const std::string& after_id, size_t limit) const;

    // Many writes as one commit (a history import), instead of one each.
    class Batch {
    public:
        explicit Batch(Store& s);
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
    private:
        Store& s_;
        bool began_ = false;  // not nested, never throwing: it runs inside network callbacks
    };

    // False if it could not be written (the post would never be sent).
    bool enqueue(const std::string& id, const std::string& payload, uint64_t now_ms);
    std::vector<OutboxItem> due(uint64_t now_ms) const;
    std::vector<OutboxItem> outbox() const;
    void sent(const std::string& id);
    // Accepted but not confirmed: not due again for `window_ms`, a window that
    // doubles with every resend (capped at an hour), so a post whose
    // confirmation never comes is resent less and less often, not every two
    // minutes forever. Returns how many times it has been resent.
    int awaiting(const std::string& id, uint64_t now_ms, uint64_t window_ms);
    // Make everything in the outbox due now (the network just came back).
    void due_now(uint64_t now_ms);
    // Record a failure and schedule the next try with exponential back-off.
    void failed(const std::string& id, const std::string& error, uint64_t now_ms);

    // Writes the key in plain text: refused (false) once the keys are sealed
    // with a passphrase (keystore.h), so no code path can put a plain key
    // back next to sealed ones.
    bool save_account(const Account& a, bool selected);
    std::vector<Account> accounts() const;
    std::optional<std::string> selected_label() const;
    void remove_account(const std::string& label);
    // Flush the write-ahead log into the database so a replaced or deleted key
    // does not linger in it (deleted content is zeroed: secure_delete is on).
    void scrub();

    // ── Keys at rest: the raw rows, for keystore.h ─────────────────────────
    struct AccountRow {
        std::string label;
        PublicKey pk{};
        SecretBytes secret;   // 64 bytes in plain text, or a sealed blob
        uint64_t created_ms = 0;
        uint32_t posts = 0;
        bool selected = false;
        bool backed_up = false;
    };
    std::vector<AccountRow> account_rows() const;
    // Insert or replace by label, secret as given.
    bool put_account_row(const AccountRow& r);
    // Everything but the secret, for a row that exists with this key.
    bool update_account_info(const Account& a, bool selected);
    // Small named values (the passphrase's salt and check value).
    std::optional<SecretBytes> meta(const std::string& key) const;
    bool set_meta(const std::string& key, const SecretBytes& value);
    bool erase_meta(const std::string& key);
    // Rewrite the file and flush its log, so overwritten keys leave no copy.
    void compact();
    // Several writes as one, rolled back unless commit() is called.
    class Txn {
    public:
        explicit Txn(Store& s);
        ~Txn();
        bool commit();
        Txn(const Txn&) = delete;
        Txn& operator=(const Txn&) = delete;
    private:
        Store& s_;
        bool open_ = false, nested_ = false;
    };

    // ── Replies to you, your posts (inbox.h) ────────────────────────────────
    // Posts by `author` in a forum, newest first, at most `limit`.
    std::vector<Stored> by_author(const std::string& forum, const PublicKey& author, size_t limit) const;
    // Posts not by `me` that reply in a topic `me` started, reply after `me`
    // replied in a topic, or carry `needle` (lower case) in their title or
    // body. Newest received first, at most `limit`.
    std::vector<Stored> inbox_candidates(const std::string& forum, const PublicKey& me, const std::string& needle,
                                         size_t limit) const;
    // How far an account has read its inbox (received time), by label.
    uint64_t inbox_read(const std::string& label) const;
    void set_inbox_read(const std::string& label, uint64_t received_ms);
    // The open database, for the parts of the core that keep tables of their
    // own in the same file (moderation, groups).
    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
};

// The back-off before retry number `attempts` (1-based): 2s, 4s, 8s … capped at 5 min.
uint64_t backoff_ms(int attempts);

} // namespace forum
