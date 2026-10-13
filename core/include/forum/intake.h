// SPDX-License-Identifier: MIT OR Apache-2.0
//
// What a node agrees to store from the network, per hour.
//
// Sending is paced by RateLimiter; this is the other side. Anyone can publish
// on a forum's topic, and every valid post that is stored costs this node disk
// space, a row in every topic list and a place in every history answer it
// gives. So a post that has passed its signature check is still admitted only
// within budgets:
//
//   - per author key, topics and replies counted apart;
//   - one shared budget for keys this node holds no post from, because an
//     anonymous post is signed by a key made for it alone, and per-key limits
//     mean nothing to an author who can make a key per post;
//   - a total for everything that arrives live, whatever the number of keys;
//   - history (a store node's archive, a peer's answer to our own request, a
//     snapshot) has its own budget, in posts and in bytes, large enough to
//     catch up a real backlog and still bounded.
//
// Every window is a sliding hour of ARRIVAL time, the clock of this node when
// the post reached it. The date a post carries is its author's choice and is
// never used here, so back-dating or reordering posts cannot reset or skip a
// budget. A refused post is not remembered as seen: it can be admitted later,
// for instance from history once the budget has room.
#pragma once

#include "forum/post.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace forum {

// Where a post came from. Live: published on the topic as we listened, or a
// bundle nobody here asked for. History: the network's store nodes, or an
// answer to a history request this node made.
enum class Origin { Live, History };

struct IntakeLimits {
    uint64_t window_ms = 60 * 60 * 1000;
    // Live, per author key.
    size_t topics_per_key = 20;
    size_t replies_per_key = 120;
    // Live, all keys this node holds no post from, together.
    size_t new_key_posts = 60;
    // Live, everything.
    size_t live_posts = 1000;
    // History, everything.
    size_t history_posts = 5000;
    size_t history_bytes = 16 * 1024 * 1024;
};

class Intake {
public:
    enum class Verdict { Ok, KeyTopics, KeyReplies, NewKeys, LiveTotal, HistoryPosts, HistoryBytes };

    explicit Intake(IntakeLimits l = {}) : lim_(l) {}

    // Whether a verified, not yet stored post may be stored, and if so, charge
    // it to the budgets. `known_key`: this node already holds a post signed by
    // that key. `bytes`: the post's encoded size. `arrival_ms`: this node's
    // clock now, never the post's date.
    Verdict admit(const Post& p, bool known_key, Origin from, uint64_t arrival_ms, size_t bytes);

    // Whether history can take anything more right now (no point asking
    // peers for pages that would be refused).
    bool history_room(uint64_t arrival_ms);

    const IntakeLimits& limits() const { return lim_; }
    uint64_t refused() const { return refused_; }
    uint64_t refused(Verdict v) const;
    // How many keys have a live window open: bounded by `live_posts`.
    size_t tracked_keys() const { return keys_.size(); }

private:
    struct Window {
        std::deque<std::pair<uint64_t, size_t>> events;  // arrival, weight
        size_t sum = 0;
        void expire(uint64_t now, uint64_t window);
        void add(uint64_t now, size_t weight) { events.emplace_back(now, weight); sum += weight; }
    };
    struct KeyWindows { Window topics, replies; };
    uint64_t clock(uint64_t arrival_ms);
    void expire_keys(uint64_t now);

    IntakeLimits lim_;
    std::map<PublicKey, KeyWindows> keys_;
    Window new_keys_, live_, history_posts_, history_bytes_;
    uint64_t last_ = 0;
    uint64_t refused_ = 0;
    std::map<Verdict, uint64_t> by_reason_;
};

} // namespace forum
