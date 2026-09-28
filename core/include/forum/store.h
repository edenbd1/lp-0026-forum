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
    // Every post of a forum, oldest first.
    std::vector<Post> all(const std::string& forum) const;

    void enqueue(const std::string& id, const std::string& payload, uint64_t now_ms);
    std::vector<OutboxItem> due(uint64_t now_ms) const;
    std::vector<OutboxItem> outbox() const;
    void sent(const std::string& id);
    // Record a failure and schedule the next try with exponential back-off.
    void failed(const std::string& id, const std::string& error, uint64_t now_ms);

    void save_account(const Account& a, bool selected);
    std::vector<Account> accounts() const;
    std::optional<std::string> selected_label() const;
    void remove_account(const std::string& label);

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
};

// The back-off before retry number `attempts` (1-based): 2s, 4s, 8s … capped at 5 min.
uint64_t backoff_ms(int attempts);

} // namespace forum
