// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/engine.h"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

#include <arpa/inet.h>

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
// A clock before 2020 is a test's clock, not a real one: no date check then.
constexpr uint64_t kRealClockMs = 1577836800000ull;
} // namespace

size_t utf8_cut(const std::string& s, size_t from, size_t max) {
    if (from >= s.size()) return 0;
    size_t n = std::min(max, s.size() - from);
    if (from + n == s.size()) return n;
    // Step back over continuation bytes (10xxxxxx) to the start of a character.
    size_t k = n;
    while (k > 0 && (static_cast<unsigned char>(s[from + k]) & 0xC0) == 0x80) --k;
    return k > 0 ? k : n;  // a single character longer than `max`: cannot help it
}

bool is_public_multiaddr(const std::string& addr) {
    // /ip4/<a>/… or /ip6/<a>/… only. DNS names are refused: they can resolve
    // to anything, including this machine ("localhost.", "127.0.0.1.nip.io").
    auto seg = [&](size_t n) {
        size_t start = 0;
        for (size_t k = 0; k <= n; ++k) {
            if (start >= addr.size() || addr[start] != '/') return std::string();
            const size_t end = addr.find('/', start + 1);
            if (k == n) return addr.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
            if (end == std::string::npos) return std::string();
            start = end;
        }
        return std::string();
    };
    const std::string proto = seg(0), host = seg(1);
    if (proto == "ip4") {
        // Strict dotted decimal: no leading zeros (octal), signs or spaces.
        unsigned v[4];
        size_t i = 0;
        for (int k = 0; k < 4; ++k) {
            if (i >= host.size() || !std::isdigit(static_cast<unsigned char>(host[i]))) return false;
            if (host[i] == '0' && i + 1 < host.size() && std::isdigit(static_cast<unsigned char>(host[i + 1]))) return false;
            unsigned x = 0;
            size_t d = 0;
            while (i < host.size() && std::isdigit(static_cast<unsigned char>(host[i]))) { x = x * 10 + (host[i++] - '0'); if (++d > 3) return false; }
            if (x > 255) return false;
            v[k] = x;
            if (k < 3) { if (i >= host.size() || host[i] != '.') return false; ++i; }
        }
        if (i != host.size()) return false;
        const unsigned a = v[0], b = v[1];
        if (a == 0 || a == 10 || a == 127 || a >= 224) return false;           // this net, private, loopback, multicast/reserved
        if (a == 169 && b == 254) return false;                                // link-local
        if (a == 172 && b >= 16 && b <= 31) return false;                      // private
        if (a == 192 && b == 168) return false;                                // private
        if (a == 100 && b >= 64 && b <= 127) return false;                     // carrier-grade NAT
        if (a == 198 && (b == 18 || b == 19)) return false;                    // benchmarking
        if (a == 192 && b == 0 && (v[2] == 0 || v[2] == 2)) return false;      // IETF protocol assignments, documentation
        if (a == 198 && b == 51 && v[2] == 100) return false;                  // documentation
        if (a == 203 && b == 0 && v[2] == 113) return false;                   // documentation
        return true;
    }
    if (proto == "ip6") {
        unsigned char b[16];
        if (host.empty() || inet_pton(AF_INET6, host.c_str(), b) != 1) return false;
        bool zero12 = true;
        for (int k = 0; k < 12; ++k) zero12 = zero12 && b[k] == 0;
        if (zero12) return false;                                              // ::, ::1, and v4-compatible ::a.b.c.d
        bool mapped = true;
        for (int k = 0; k < 10; ++k) mapped = mapped && b[k] == 0;
        if (mapped && b[10] == 0xff && b[11] == 0xff) return false;            // v4-mapped ::ffff:a.b.c.d
        bool zero8 = true;
        for (int k = 0; k < 8; ++k) zero8 = zero8 && b[k] == 0;
        if (zero8 && b[8] == 0xff && b[9] == 0xff) return false;               // v4-translated ::ffff:0:a.b.c.d
        if (b[0] == 0x20 && b[1] == 0x02) return false;                        // 6to4 2002::/16 embeds any IPv4
        if (b[0] == 0x01 && b[1] == 0x00 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0) return false;  // discard 100::/64
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) return false;   // Teredo 2001::/32 embeds any IPv4
        if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;   // NAT64 64:ff9b::/96
        if ((b[0] & 0xfe) == 0xfc) return false;                               // unique local fc00::/7
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false;               // link-local fe80::/10
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return false;               // site-local fec0::/10
        if (b[0] == 0xff) return false;                                        // multicast
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;   // documentation
        return true;
    }
    return false;
}

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
    if (!store_.enqueue(id, encode(signed_post), now_ms)) return {};  // could not be kept to send: refuse it
    if (store_.put(signed_post, now_ms) && on_post) on_post(signed_post, id);
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
    // History answers share the rate limit with what the user writes.
    while (!answers_.empty() && limiter_.take(now_ms)) {
        net_.send(topic_, answers_.front());
        answers_.pop_front();
        ++sent;
    }
    for (const auto& item : store_.due(now_ms)) {
        if (!limiter_.take(now_ms)) break;  // paced, not dropped: it stays due
        const SendResult r = net_.send(topic_, item.payload);
        if (r.ok) {
            if (r.confirmed) store_.sent(item.id);
            else store_.awaiting(item.id, now_ms, kConfirmWindowMs);
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
            const size_t ours = store_.count(forum_);
            // A Storage request, when this node does not answer those (it keeps
            // its storage address to itself), is answered over Delivery instead.
            const int path = str_at(a, "via") == "delivery" || !on_history_wanted ? 1 : 0;
            const uint64_t every = path == 1 ? kDeliveryAnswerEveryMs : kAnswerEveryMs;
            const bool quiet = !answered_[path] || now_ms >= last_answer_ms_[path] + every;
            if (quiet && !pending_ && theirs >= 0 && static_cast<size_t>(theirs) < ours) {
                HistoryRequest r;
                r.id = str_at(a, "id").substr(0, 64);
                r.have = static_cast<size_t>(theirs);
                r.via_delivery = path == 1;
                const int64_t since = int_at(a, "since"), until = int_at(a, "until");
                r.since_ms = since > 0 ? static_cast<uint64_t>(since) : 0;
                r.until_ms = until > 0 ? static_cast<uint64_t>(until) : 0;
                r.until_id = str_at(a, "until_id").substr(0, 64);
                pending_ = r;
                pending_due_ms_ = now_ms + (jitter ? jitter() : randombytes_uniform(kAnswerJitterMs));
            }
            return false;
        }
        // History sent as bundles over Delivery: the snapshot format, carried
        // inline. Every post in it is checked as if it had arrived alone.
        if (a.is_object() && int_at(a, kSnapshotTag) == 1 && str_at(a, "forum") == forum_) {
            std::vector<Post> valid;
            const int added = import_snapshot(payload, now_ms, &valid);
            const std::string re = str_at(a, "re");
            // Someone else answered the request we were about to answer: stand
            // down only if they sent everything our own first page would have
            // carried. Anything less (empty, forged, a replay of a few posts)
            // must not silence the honest answerers; the whole page is harmless.
            if (pending_ && pending_->via_delivery && !re.empty() && pending_->id == re) {
                bool covers = true;
                for (const auto& id : first_page_ids(*pending_, now_ms)) {
                    bool in = false;
                    for (const auto& p : valid) in = in || p.id() == id;
                    covers = covers && in;
                }
                if (covers) pending_.reset();
            }
            // Our own request: note the answer, and if it says there is more,
            // where the next page starts. That is worked out from the verified
            // posts in it, never taken from the unsigned fields.
            const auto mine = asked_.find(re);
            if (mine != asked_.end() && now_ms <= mine->second.closes_ms && !valid.empty()) {
                mine->second.answered = true;
                const auto more = a.find("more");
                if (more != a.end() && more->is_boolean() && more->get<bool>()) {
                    mine->second.more = true;
                    const Post* last = &valid.front();
                    for (const auto& p : valid)
                        if (p.ts_ms < last->ts_ms || (p.ts_ms == last->ts_ms && p.id() > last->id())) last = &p;
                    // Of several answers, the least advanced cursor (newest post) wins.
                    const bool first = !follow_up_for_ || *follow_up_for_ != re;
                    const bool higher = last->ts_ms > follow_up_.ts || (last->ts_ms == follow_up_.ts && last->id() < follow_up_.id);
                    if ((first || higher) && follow_ups_ < kMaxFollowUps) {
                        follow_up_for_ = re;
                        follow_up_ = Cursor{true, mine->second.since_ms, last->ts_ms, last->id()};
                        if (first) follow_up_due_ms_ = now_ms + kDeliveryAnswerEveryMs + 1000;
                    }
                }
            }
            return added > 0;
        }
        // An announcement answers a Storage request only.
        if (a.is_object() && int_at(a, kAnnounceTag) == 1 && str_at(a, "forum") == forum_ && pending_ && !pending_->via_delivery)
            stand_down(str_at(a, "re"));
        if (on_snapshot && a.is_object() && int_at(a, kAnnounceTag) == 1 && str_at(a, "forum") == forum_) {
            // Only an answer to a request of ours, while it is open, and once.
            const auto open = asked_.find(str_at(a, "re"));
            if (open == asked_.end() || now_ms > open->second.closes_ms) return false;
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
                asked_.erase(open);
                on_snapshot(an);
            }
        }
        return false;
    }
    return accept(*p, now_ms);
}

bool Engine::accept(const Post& p, uint64_t now_ms) {
    if (p.forum != forum_) return false;
    if (now_ms >= kRealClockMs && p.ts_ms > now_ms + kMaxClockSkewMs) return false;  // dated in the future
    // A reply to a topic we do not have yet is still kept: the topic may arrive
    // later, from another peer or from history, and the reply attaches then.
    if (!store_.put(p, now_ms)) return false;
    if (on_post) on_post(p, p.id());
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

int Engine::import_snapshot(const std::string& doc, uint64_t now_ms, std::vector<Post>* valid) {
    const json s = json::parse(doc, nullptr, false);
    if (!s.is_object() || int_at(s, kSnapshotTag) != 1 || str_at(s, "forum") != forum_) return 0;
    const auto it = s.find("posts");
    if (it == s.end() || !it->is_array() || it->size() > kMaxSnapshotPosts) return 0;
    // Posts only: a snapshot is a bundle of signed posts, and nothing else in
    // it (an announcement, a request) is acted on. One commit for the lot.
    Store::Batch batch(store_);
    int added = 0;
    for (const auto& m : *it) {
        if (!m.is_string()) continue;
        const auto p = decode(m.get<std::string>());
        if (p && p->forum == forum_ && valid) valid->push_back(*p);
        if (p && accept(*p, now_ms)) ++added;
    }
    return added;
}

std::string Engine::request_history(uint64_t now_ms, bool via_delivery) {
    if (!session_start_ms_) session_start_ms_ = now_ms;
    uint64_t since = 0;
    if (via_delivery) {
        // Ask for what is newer than what we held when this session began,
        // with an hour's margin for clocks and posts that arrived out of order.
        // Not the newest post now: after a partial answer that would skip the
        // older part of what was missed. A post dated after now (kept from
        // before dates were checked) does not count either.
        const uint64_t limit = now_ms >= kRealClockMs ? now_ms + kMaxClockSkewMs : static_cast<uint64_t>(INT64_MAX);
        const uint64_t newest = store_.newest_ts(forum_, limit, session_start_ms_);
        since = newest > 3600000 ? newest - 3600000 : 0;
        // Part-way through a backlog: carry on from where the pages stopped.
        if (cursor_.active) return send_request(now_ms, true, cursor_.since_ms, cursor_.ts, cursor_.id);
        follow_ups_ = 0;
    }
    return send_request(now_ms, via_delivery, since, 0);
}

std::string Engine::send_request(uint64_t now_ms, bool via_delivery, uint64_t since_ms, uint64_t until_ms,
                                 const std::string& until_id) {
    uint8_t raw[8];
    randombytes_buf(raw, sizeof raw);
    const std::string id = to_hex(raw, sizeof raw);
    for (auto it = asked_.begin(); it != asked_.end();) {
        if (now_ms <= it->second.closes_ms) { ++it; continue; }
        // A paged request answered without "more": the backlog is done.
        if (it->second.paged && it->second.answered && !it->second.more) cursor_.active = false;
        it = asked_.erase(it);
    }
    asked_[id] = Asked{now_ms + kRequestOpenMs, since_ms, until_ms != 0};
    json w{{kWantTag, 1}, {"forum", forum_}, {"have", store_.count(forum_)}, {"id", id}};
    if (via_delivery) {
        w["via"] = "delivery";
        w["since"] = since_ms;
        if (until_ms) {
            w["until"] = until_ms;
            w["until_id"] = until_id;
        }
    }
    net_.send(topic_, w.dump());
    return id;
}

void Engine::stand_down(const std::string& re) {
    if (pending_ && !re.empty() && pending_->id == re) pending_.reset();  // someone else answered
}

void Engine::tick(uint64_t now_ms) {
    if (follow_up_for_ && now_ms >= follow_up_due_ms_) {
        follow_up_for_.reset();
        ++follow_ups_;
        cursor_ = follow_up_;
        send_request(now_ms, true, cursor_.since_ms, cursor_.ts, cursor_.id);
    }
    if (!pending_ || now_ms < pending_due_ms_) return;
    const HistoryRequest r = *pending_;
    pending_.reset();
    answered_[r.via_delivery] = true;
    last_answer_ms_[r.via_delivery] = now_ms;
    if (r.via_delivery) {
        // Capped per answer and per hour whatever was asked, and paced by
        // pump() like any other send.
        if (now_ms >= budget_since_ms_ + 3600000) {
            budget_since_ms_ = now_ms;
            budget_used_ = 0;
        }
        const size_t cap = std::min(kMaxAnswerBytes, kAnswerBudgetBytes - std::min(budget_used_, kAnswerBudgetBytes));
        size_t bytes = 0, sent = 0;
        // A cursor at or beyond the date limit is the same as starting from the top.
        const bool top = !r.until_ms || r.until_ms > upper(0, now_ms);
        auto all = top ? bundles(r.since_ms, r.id, upper(0, now_ms), "") : bundles(r.since_ms, r.id, r.until_ms, r.until_id);
        for (auto& b : all) {
            if (bytes + b.size() > cap) break;
            bytes += b.size();
            answers_.push_back(std::move(b));
            ++sent;
        }
        budget_used_ += bytes;
        // Cut short: say so, so the asker can ask for the page below.
        if (sent > 0 && sent < all.size()) {
            json last = json::parse(answers_.back(), nullptr, false);
            last["more"] = true;
            budget_used_ += 16;
            answers_.back() = last.dump();
        }
    } else if (on_history_wanted) {
        on_history_wanted(r);
    }
}

uint64_t Engine::upper(uint64_t, uint64_t now_ms) const {
    // Everything dated up to now + skew (a test clock: everything).
    return now_ms >= kRealClockMs ? now_ms + kMaxClockSkewMs + 1 : static_cast<uint64_t>(INT64_MAX);
}

std::vector<std::string> Engine::first_page_ids(const HistoryRequest& r, uint64_t now_ms) const {
    const bool top = !r.until_ms || r.until_ms > upper(0, now_ms);
    std::vector<std::string> ids;
    size_t bytes = 0;
    for (const auto& p : store_.recent(forum_, r.since_ms, top ? upper(0, now_ms) : r.until_ms, top ? "" : r.until_id,
                                       kMaxFallbackPosts)) {
        const size_t n = encode(p).size() + 3;
        if (bytes + n > kBundleBytes && !ids.empty()) break;
        bytes += n;
        ids.push_back(p.id());
    }
    return ids;
}

std::vector<std::string> Engine::bundles(uint64_t since_ms, const std::string& re, uint64_t until_ms,
                                         const std::string& until_id) const {
    // The most recent first, so a cap keeps what a returning reader wants,
    // from the cursor down. Never a post dated in the future.
    const uint64_t before = until_ms ? until_ms : static_cast<uint64_t>(INT64_MAX);
    const std::vector<Post> posts = store_.recent(forum_, since_ms, before, until_ms ? until_id : std::string(), kMaxFallbackPosts);
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
