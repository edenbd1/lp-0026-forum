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
constexpr const char* kWantTag = "logos-forum-want";
constexpr size_t kMaxSnapshotPosts = 100000;

// Typed reads that never throw: anything on the topic can be sent by anyone,
// and json::value() throws when a field exists with another type.
std::string str_at(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
int64_t int_at(const json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}
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

std::string pubsub_topic(const std::string& ct, int cluster, int shards) {
    // /app/version/name/encoding
    std::vector<std::string> parts;
    size_t i = 1;
    while (i <= ct.size()) {
        const size_t j = ct.find('/', i);
        parts.push_back(ct.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (parts.size() < 2 || shards <= 0) return {};
    const std::string bytes = parts[0] + parts[1];
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    uint64_t v = 0;
    for (int k = 24; k < 32; ++k) v = (v << 8) | h[k];
    return "/waku/2/rs/" + std::to_string(cluster) + "/" + std::to_string(v % static_cast<uint64_t>(shards));
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
            if (r.confirmed) store_.sent(item.id);
            else store_.awaiting(item.id, now_ms + kConfirmWindowMs);
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
        if (a.is_object() && int_at(a, kWantTag) == 1 && str_at(a, "forum") == forum_) {
            const int64_t theirs = int_at(a, "have");
            const size_t ours = store_.all(forum_).size();
            const bool quiet = !answered_ || now_ms >= last_answer_ms_ + kAnswerEveryMs;
            const bool can = str_at(a, "via") == "delivery" || static_cast<bool>(on_history_wanted);
            if (can && quiet && !pending_ && theirs >= 0 && static_cast<size_t>(theirs) < ours) {
                HistoryRequest r;
                r.id = str_at(a, "id").substr(0, 64);
                r.have = static_cast<size_t>(theirs);
                r.via_delivery = str_at(a, "via") == "delivery";
                const int64_t since = int_at(a, "since");
                r.since_ms = since > 0 ? static_cast<uint64_t>(since) : 0;
                pending_ = r;
                pending_due_ms_ = now_ms + (jitter ? jitter() : randombytes_uniform(kAnswerJitterMs));
            }
            return false;
        }
        // History sent as bundles over Delivery: the snapshot format, carried
        // inline. Every post in it is checked as if it had arrived alone.
        if (a.is_object() && int_at(a, kSnapshotTag) == 1 && str_at(a, "forum") == forum_) {
            stand_down(str_at(a, "re"));
            return import_snapshot(payload, now_ms) > 0;
        }
        if (a.is_object() && int_at(a, kAnnounceTag) == 1 && str_at(a, "forum") == forum_)
            stand_down(str_at(a, "re"));
        if (on_snapshot && a.is_object() && int_at(a, kAnnounceTag) == 1 && str_at(a, "forum") == forum_) {
            const std::string cid = str_at(a, "cid");
            const int64_t n = int_at(a, "posts");
            if (!cid.empty() && cid.size() <= 128) {
                Announcement an{cid, n > 0 ? static_cast<size_t>(n) : 0, str_at(a, "peer"), {}};
                if (an.peer.size() > 128) an.peer.clear();
                const auto it = a.find("addrs");
                if (!an.peer.empty() && it != a.end() && it->is_array())
                    for (const auto& x : *it)
                        if (x.is_string() && x.get<std::string>().size() <= 256 && an.addrs.size() < 8)
                            an.addrs.push_back(x.get<std::string>());
                on_snapshot(an);
            }
        }
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

void Engine::confirm(const std::string& id) { store_.sent(id); }

int Engine::reconnected(uint64_t now_ms) {
    store_.due_now(now_ms);
    return pump(now_ms);
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
    if (!s.is_object() || int_at(s, kSnapshotTag) != 1 || str_at(s, "forum") != forum_) return 0;
    const auto it = s.find("posts");
    if (it == s.end() || !it->is_array() || it->size() > kMaxSnapshotPosts) return 0;
    int added = 0;
    for (const auto& m : *it)
        if (m.is_string() && receive(m.get<std::string>(), now_ms)) ++added;
    return added;
}

std::string Engine::request_history(uint64_t, bool via_delivery) {
    uint8_t raw[8];
    randombytes_buf(raw, sizeof raw);
    const std::string id = to_hex(raw, sizeof raw);
    json w{{kWantTag, 1}, {"forum", forum_}, {"have", store_.all(forum_).size()}, {"id", id}};
    if (via_delivery) {
        // Ask only for what is newer than what we hold, with an hour's margin
        // for clocks and for posts that arrived out of order.
        uint64_t newest = 0;
        for (const auto& p : store_.all(forum_)) newest = std::max(newest, p.ts_ms);
        w["via"] = "delivery";
        w["since"] = newest > 3600000 ? newest - 3600000 : 0;
    }
    net_.send(topic_, w.dump());
    return id;
}

void Engine::stand_down(const std::string& re) {
    if (pending_ && !re.empty() && pending_->id == re) pending_.reset();  // someone else answered
}

void Engine::tick(uint64_t now_ms) {
    if (!pending_ || now_ms < pending_due_ms_) return;
    const HistoryRequest r = *pending_;
    pending_.reset();
    answered_ = true;
    last_answer_ms_ = now_ms;
    if (r.via_delivery) {
        for (const auto& b : bundles(r.since_ms, r.id)) net_.send(topic_, b);
    } else if (on_history_wanted) {
        on_history_wanted(r);
    }
}

std::vector<std::string> Engine::bundles(uint64_t since_ms, const std::string& re) const {
    std::vector<Post> posts;
    for (const auto& p : store_.all(forum_))
        if (p.ts_ms >= since_ms) posts.push_back(p);
    // The most recent first, so a cap keeps what a returning reader wants.
    std::sort(posts.begin(), posts.end(), [](const Post& a, const Post& b) { return a.ts_ms > b.ts_ms; });
    if (posts.size() > kMaxFallbackPosts) posts.resize(kMaxFallbackPosts);
    std::vector<std::string> out;
    json cur = json::array();
    size_t bytes = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        out.push_back(json{{kSnapshotTag, 1}, {"forum", forum_}, {"re", re}, {"posts", cur}}.dump());
        cur = json::array();
        bytes = 0;
    };
    for (const auto& p : posts) {
        std::string e = encode(p);
        if (bytes + e.size() > kBundleBytes) flush();
        bytes += e.size() + 3;
        cur.push_back(std::move(e));
    }
    flush();
    return out;
}

void Engine::announce_snapshot(const std::string& cid, size_t posts, uint64_t, const std::string& re) {
    // Sent directly rather than through the outbox: an announcement that does
    // not go out is simply made again at the next snapshot.
    json a{{kAnnounceTag, 1}, {"forum", forum_}, {"cid", cid}, {"posts", posts}};
    if (!re.empty()) a["re"] = re;
    if (!provider_peer_.empty()) {
        a["peer"] = provider_peer_;
        a["addrs"] = provider_addrs_;
    }
    net_.send(topic_, a.dump());
}

} // namespace forum
