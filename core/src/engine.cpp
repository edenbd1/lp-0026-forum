// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/engine.h"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace forum {
namespace {
using json = nlohmann::json;
constexpr const char* kSnapshotTag = "logos-forum-snapshot";
constexpr const char* kAnnounceTag = "logos-forum-snapshot-at";
constexpr size_t kMaxSnapshotPosts = 100000;
} // namespace

bool RateLimiter::take(uint64_t now_ms) {
    if (!started_) {
        started_ = true;
        last_ = now_ms;
    }
    if (now_ms > last_) {
        tokens_ = std::min(cap_, tokens_ + (now_ms - last_) * rate_);
        last_ = now_ms;
    }
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
}

std::string content_topic(const std::string& forum) {
    // Logos Delivery content topics take the form /app/version/name/encoding.
    // The forum name is folded to a safe slug and suffixed with a short hash of
    // the exact name, so "Logos Forum" and "logos-forum" stay distinct forums.
    std::string slug;
    for (char c : forum) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) slug.push_back(static_cast<char>(std::tolower(u)));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(forum.data()), forum.size());
    return "/logos-forum/1/" + (slug.empty() ? std::string("forum") : slug) + "-" + to_hex(h, 4) + "/json";
}

Engine::Engine(Store& store, Transport& net, std::string forum)
    : store_(store), net_(net), forum_(std::move(forum)), topic_(content_topic(forum_)) {
    net_.subscribe(topic_);
}

std::string Engine::compose(Post p, Account* as, const std::string& alias, uint64_t now_ms) {
    p.forum = forum_;
    p.ts_ms = now_ms;
    const Post signed_post = as ? author_as(*as, std::move(p), alias) : author_anonymously(std::move(p));
    const std::string id = signed_post.id();
    if (store_.put(signed_post, now_ms) && on_post) on_post(signed_post, id);
    store_.enqueue(id, encode(signed_post), now_ms);
    return id;
}

std::string Engine::post_topic(Account* as, const std::string& title, const std::string& body,
                               const std::string& alias, uint64_t now_ms) {
    Post p;
    p.kind = Kind::Topic;
    p.title = title;
    p.body = body;
    return compose(std::move(p), as, alias, now_ms);
}

std::string Engine::post_reply(Account* as, const std::string& topic_id, const std::string& body,
                               const std::string& alias, uint64_t now_ms) {
    Post p;
    p.kind = Kind::Reply;
    p.topic_id = topic_id;
    p.body = body;
    return compose(std::move(p), as, alias, now_ms);
}

int Engine::pump(uint64_t now_ms) {
    int sent = 0;
    for (const auto& item : store_.due(now_ms)) {
        if (!limiter_.take(now_ms)) break;  // paced, not dropped: it stays due
        const SendResult r = net_.send(topic_, item.payload);
        if (r.ok) {
            store_.sent(item.id);
            if (on_sent) on_sent(item.id, r.request_id);
            ++sent;
        } else {
            store_.failed(item.id, r.error.empty() ? "send failed" : r.error, now_ms);
        }
    }
    return sent;
}

bool Engine::receive(const std::string& payload, uint64_t now_ms) {
    const auto p = decode(payload);
    if (!p) {
        // Not a post: perhaps a peer pointing at a history snapshot. It is
        // unsigned on purpose — it carries no authority, only a place to look,
        // and everything fetched from there is verified post by post.
        const json a = json::parse(payload, nullptr, false);
        if (on_snapshot && a.is_object() && a.value(kAnnounceTag, 0) == 1 && a.value("forum", "") == forum_ &&
            a.contains("cid") && a["cid"].is_string() && a["cid"].get<std::string>().size() <= 128)
            on_snapshot(a["cid"].get<std::string>(), a.value("posts", size_t{0}));
        return false;
    }
    if (p->forum != forum_) return false;
    // A reply to a topic we do not have yet is still kept: the topic may arrive
    // later, from another peer or from history, and the reply attaches then.
    if (!store_.put(*p, now_ms)) return false;
    if (on_post) on_post(*p, p->id());
    return true;
}

int Engine::catch_up(uint64_t now_ms) {
    int added = 0;
    for (const auto& m : net_.history(topic_))
        if (receive(m, now_ms)) ++added;
    return added;
}

bool Engine::requeue(const std::string& id, const std::string& error, uint64_t now_ms) {
    const auto p = store_.get(id);
    if (!p) return false;
    store_.enqueue(id, encode(*p), now_ms);
    store_.failed(id, error, now_ms);
    return true;
}

std::string Engine::snapshot() const {
    json posts = json::array();
    for (const auto& p : store_.all(forum_)) posts.push_back(encode(p));
    return json{{kSnapshotTag, 1}, {"forum", forum_}, {"posts", posts}}.dump();
}

int Engine::import_snapshot(const std::string& doc, uint64_t now_ms) {
    const json s = json::parse(doc, nullptr, false);
    if (!s.is_object() || s.value(kSnapshotTag, 0) != 1 || s.value("forum", "") != forum_) return 0;
    const auto it = s.find("posts");
    if (it == s.end() || !it->is_array() || it->size() > kMaxSnapshotPosts) return 0;
    int added = 0;
    for (const auto& m : *it)
        if (m.is_string() && receive(m.get<std::string>(), now_ms)) ++added;
    return added;
}

void Engine::announce_snapshot(const std::string& cid, size_t posts, uint64_t) {
    // Sent directly rather than through the outbox: an announcement that does
    // not go out is simply made again at the next snapshot.
    net_.send(topic_, json{{kAnnounceTag, 1}, {"forum", forum_}, {"cid", cid}, {"posts", posts}}.dump());
}

} // namespace forum
