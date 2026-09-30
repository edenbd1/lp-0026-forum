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
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forum {

struct SendResult {
    bool ok = false;
    std::string error;
    std::string request_id;  // what the network calls this send, if anything
    // True when the transport knows the message is out (a synchronous
    // network). False when it has only accepted it: Logos Delivery takes a send
    // and then retries it in memory, so acceptance proves nothing — the post
    // stays in the outbox until confirm() or requeue(), and is sent again if
    // neither comes within kConfirmWindowMs (also after a restart).
    bool confirmed = false;
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

// The relay shard a content topic lands on under autosharding (RFC 51):
// SHA-256 of the topic's application and version, last 8 bytes, modulo the
// cluster's shard count. Store queries need it as their pubsub topic.
std::string pubsub_topic(const std::string& content_topic, int cluster = 2, int shards = 8);

// The length of the longest prefix of `s` from `from`, at most `max` bytes,
// that ends on a UTF-8 character boundary: slicing a document into chunks must
// never cut a character in two.
size_t utf8_cut(const std::string& s, size_t from, size_t max);

// Whether a multiaddr names a publicly routable host. Loopback, private, link-
// local and carrier-grade NAT ranges are not: an announcement is unsigned, and
// must not be able to make every reader connect into its own machine or LAN.
// DNS names are accepted (they resolve to whatever the name's owner chose,
// like any public address).
bool is_public_multiaddr(const std::string& addr);

// Where a snapshot lives. The storage peer is optional: when present, a
// fetcher dials it directly instead of relying on the DHT, which finds nothing
// when both ends are behind NAT.
struct Announcement {
    std::string cid;
    size_t posts = 0;
    std::string peer;                 // Logos Storage peer id of the provider
    std::vector<std::string> addrs;   // its multiaddrs
};

// A peer asking for history. `via_delivery` means Logos Storage did not work
// for it (typically: both storage nodes behind NAT), so the answer has to come
// as bundles on the forum's Delivery topic instead of a snapshot to download.
struct HistoryRequest {
    std::string id;        // lets the other peers see it has been answered
    size_t have = 0;       // how many posts the asker holds
    bool via_delivery = false;
    uint64_t since_ms = 0; // the asker's newest post, less a margin
};

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
    // valid post for this forum. A post dated more than kMaxClockSkewMs in the
    // future is refused: its author chose the date, and a far-future one would
    // pin it at the top and poison history requests. A snapshot announcement
    // is passed to `on_snapshot` only if it answers a request this node made.
    bool receive(const std::string& payload, uint64_t now_ms);
    static constexpr uint64_t kMaxClockSkewMs = 10 * 60 * 1000;

    // Pull history from the network and merge it. Returns how many new posts.
    int catch_up(uint64_t now_ms);

    // The network confirmed a send: the post leaves the outbox.
    void confirm(const std::string& id);

    // The network came back: send what is waiting now rather than at its
    // scheduled retry. Still paced by the rate limit.
    int reconnected(uint64_t now_ms);
    static constexpr uint64_t kConfirmWindowMs = 120000;

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
    // `re` names the history request this answers, so other peers stand down.
    void announce_snapshot(const std::string& cid, size_t posts, uint64_t now_ms, const std::string& re = {});
    // Who to name as the snapshot's provider in announcements.
    void set_storage_provider(std::string peer, std::vector<std::string> addrs) {
        provider_peer_ = std::move(peer);
        provider_addrs_ = std::move(addrs);
    }

    // Ask peers for history. The network's store nodes may keep nothing (the
    // logos.test fleet keeps no archive), so a node back from offline asks the
    // forum itself. First through Logos Storage (a peer announces a snapshot);
    // if that cannot be fetched, again with `via_delivery`, and a peer sends
    // the posts themselves as bundles on the Delivery topic, which crosses NAT.
    // Carries only a count and a timestamp, nothing identifying. Returns the id.
    std::string request_history(uint64_t now_ms, bool via_delivery = false);

    // A peer asked for history over Logos Storage and holds fewer posts than
    // we do: the embedder answers with announce_snapshot(…, req.id).
    // Delivery-path requests are answered by the engine itself.
    //
    // Nobody answers at once. Every peer that could answer waits a random
    // 0…kAnswerJitterMs (call tick() to let the wait run out) and stands down
    // if it sees another peer's answer to the same request first — so one
    // newcomer costs the network one answer, not one per peer. A peer also
    // answers at most once per kAnswerEveryMs.
    std::function<void(const HistoryRequest&)> on_history_wanted;
    void tick(uint64_t now_ms);
    std::function<uint64_t()> jitter;  // default: uniform random in [0, kAnswerJitterMs)
    static constexpr uint64_t kAnswerJitterMs = 3000;
    // Delivery-path bundles: at most this many recent posts, in messages under
    // the network's size limit (logos.test: 150 KiB).
    static constexpr size_t kMaxFallbackPosts = 1000;
    static constexpr size_t kBundleBytes = 100 * 1024;
    // What one Delivery answer may put on the network, whatever was asked:
    // the most recent posts up to this size. The asker can ask again later.
    static constexpr size_t kMaxAnswerBytes = 256 * 1024;
    // How long our own history request stays open for answers.
    static constexpr uint64_t kRequestOpenMs = 2 * 60 * 1000;
    std::vector<std::string> bundles(uint64_t since_ms, const std::string& re) const;
    static constexpr uint64_t kAnswerEveryMs = 30000;
    // Delivery answers carry the posts themselves, so they are rarer.
    static constexpr uint64_t kDeliveryAnswerEveryMs = 60000;

    // Called for every new post, local or remote, after it is stored.
    std::function<void(const Post&, const std::string& id)> on_post;

    // Called when a post leaves the outbox, with the network's request id.
    std::function<void(const std::string& id, const std::string& request_id)> on_sent;

    // Called when a peer announces a snapshot in answer to one of our own
    // open history requests (announcements are unsigned: an unsolicited one
    // is ignored, so nobody can make every reader fetch from them). The
    // embedder fetches it from Logos Storage and hands the bytes to
    // import_snapshot(). Called at most once per request.
    std::function<void(const Announcement&)> on_snapshot;

private:
    std::string compose(Post p, Account* as, const std::string& alias, uint64_t now_ms);
    bool accept(const Post& p, uint64_t now_ms);
    // Our own open history requests, by id, with when they close.
    std::map<std::string, uint64_t> asked_;
    // Delivery answers waiting for the rate limit, like the outbox.
    std::deque<std::string> answers_;
    Store& store_;
    Transport& net_;
    std::string forum_, topic_;
    RateLimiter limiter_{5, 20};
    // Rate limits, one per path: a Delivery fallback usually follows a Storage
    // answer by seconds (the snapshot could not be fetched), so a shared limit
    // would silence exactly the answer that works.
    uint64_t last_answer_ms_[2] = {0, 0};
    std::optional<HistoryRequest> pending_;
    uint64_t pending_due_ms_ = 0;
    void stand_down(const std::string& re);
    std::string provider_peer_;
    std::vector<std::string> provider_addrs_;
    bool answered_[2] = {false, false};
};

} // namespace forum
