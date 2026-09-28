// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The forum's behaviour, independent of how bytes move.
//
// The engine owns the rules — what is accepted, what is stored, what is sent
// and when — and talks to the network only through `Transport`. In Basecamp
// that is Logos Delivery; in the tests it is a fake network that can be cut.
// Keeping the two apart is what lets every reliability rule be tested without a
// node: a post written offline is kept, a failed send is retried with back-off,
// history fetched on reconnect is merged without duplicates, and a burst of
// posts is paced instead of flooding the topic.
#pragma once

#include "forum/identity.h"
#include "forum/post.h"
#include "forum/store.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace forum {

struct SendResult {
    bool ok = false;
    std::string error;
    std::string request_id;  // what the network calls this send, if anything
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual SendResult send(const std::string& content_topic, const std::string& payload) = 0;
    virtual bool subscribe(const std::string& content_topic) = 0;
    // Past messages on a topic, for catching up after being offline. May be
    // empty; the engine treats history as best-effort.
    virtual std::vector<std::string> history(const std::string& content_topic) = 0;
};

// Token bucket: `capacity` sends at once, refilled at `per_minute`.
class RateLimiter {
public:
    RateLimiter(double capacity, double per_minute) : cap_(capacity), rate_(per_minute / 60000.0), tokens_(capacity) {}
    bool take(uint64_t now_ms);
private:
    double cap_, rate_, tokens_;
    uint64_t last_ = 0;
    bool started_ = false;
};

// The Logos Delivery content topic of a forum. One topic per forum keeps a
// forum's traffic, and its history, in one place.
std::string content_topic(const std::string& forum);

class Engine {
public:
    Engine(Store& store, Transport& net, std::string forum);

    const std::string& forum() const { return forum_; }
    const std::string& topic() const { return topic_; }

    // Compose. The post is stored and queued at once, so it shows up locally
    // and survives a restart even when the network is down. Returns its id.
    std::string post_topic(Account* as, const std::string& title, const std::string& body,
                           const std::string& alias, uint64_t now_ms);
    std::string post_reply(Account* as, const std::string& topic_id, const std::string& body,
                           const std::string& alias, uint64_t now_ms);

    // Send what is due in the outbox, within the rate limit. Returns how many
    // went out.
    int pump(uint64_t now_ms);

    // A message arrived on the forum's topic. Returns true if it was a new,
    // valid post for this forum. A snapshot announcement is passed to
    // `on_snapshot` and returns false.
    bool receive(const std::string& payload, uint64_t now_ms);

    // Pull history from the network and merge it. Returns how many new posts.
    int catch_up(uint64_t now_ms);

    // A send the network accepted and later reported lost: put the post back
    // in the outbox, with back-off, so it is retried like any other failure.
    // False if the post is not ours to resend (unknown id).
    bool requeue(const std::string& id, const std::string& error, uint64_t now_ms);

    // History snapshots for Logos Storage. `snapshot()` is every post of this
    // forum as one document; `import_snapshot()` merges one, checking every
    // post in it exactly as if it had arrived live — a snapshot is a bundle of
    // signed posts, not a source of trust. Returns how many were new.
    std::string snapshot() const;
    int import_snapshot(const std::string& doc, uint64_t now_ms);

    // Announce a snapshot stored under `cid` on the forum's topic, so peers who
    // were away for longer than the network keeps history can fetch it.
    void announce_snapshot(const std::string& cid, size_t posts, uint64_t now_ms);

    // Called for every new post, local or remote, after it is stored.
    std::function<void(const Post&, const std::string& id)> on_post;

    // Called when a post leaves the outbox, with the network's request id.
    std::function<void(const std::string& id, const std::string& request_id)> on_sent;

    // Called when a peer announces a snapshot. The embedder fetches it from
    // Logos Storage and hands the bytes to import_snapshot().
    std::function<void(const std::string& cid, size_t posts)> on_snapshot;

private:
    std::string compose(Post p, Account* as, const std::string& alias, uint64_t now_ms);
    Store& store_;
    Transport& net_;
    std::string forum_, topic_;
    RateLimiter limiter_{5, 20};
};

} // namespace forum
