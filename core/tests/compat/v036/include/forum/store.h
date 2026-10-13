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

    void save_account(const Account& a, bool selected);
    std::vector<Account> accounts() const;
    std::optional<std::string> selected_label() const;
    void remove_account(const std::string& label);
    // Flush the write-ahead log into the database so a replaced or deleted key
    // does not linger in it (deleted content is zeroed: secure_delete is on).
    void scrub();

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
};

// The back-off before retry number `attempts` (1-based): 2s, 4s, 8s … capped at 5 min.
uint64_t backoff_ms(int attempts);

} // namespace forum
