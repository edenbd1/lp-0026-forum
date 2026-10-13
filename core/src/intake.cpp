// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/intake.h"

#include <algorithm>

namespace forum {

void Intake::Window::expire(uint64_t now, uint64_t window) {
    // An event stays in the window for `window` ms after it arrived.
    while (!events.empty() && events.front().first + window <= now) {
        sum -= events.front().second;
        events.pop_front();
    }
}

uint64_t Intake::clock(uint64_t arrival_ms) {
    // This node's clock only. If it steps back, the windows do not reopen:
    // time as the budgets see it never goes backwards.
    last_ = std::max(last_, arrival_ms);
    return last_;
}

void Intake::expire_keys(uint64_t now) {
    for (auto it = keys_.begin(); it != keys_.end();) {
        it->second.topics.expire(now, lim_.window_ms);
        it->second.replies.expire(now, lim_.window_ms);
        if (it->second.topics.events.empty() && it->second.replies.events.empty()) it = keys_.erase(it);
        else ++it;
    }
}

bool Intake::history_room(uint64_t arrival_ms) {
    const uint64_t now = clock(arrival_ms);
    history_posts_.expire(now, lim_.window_ms);
    history_bytes_.expire(now, lim_.window_ms);
    return history_posts_.sum < lim_.history_posts && history_bytes_.sum < lim_.history_bytes;
}

uint64_t Intake::refused(Verdict v) const {
    const auto it = by_reason_.find(v);
    return it == by_reason_.end() ? 0 : it->second;
}

Intake::Verdict Intake::admit(const Post& p, bool known_key, Origin from, uint64_t arrival_ms, size_t bytes) {
    const uint64_t now = clock(arrival_ms);
    auto refuse = [&](Verdict v) {
        ++refused_;
        ++by_reason_[v];
        return v;
    };
    if (from == Origin::History) {
        history_posts_.expire(now, lim_.window_ms);
        history_bytes_.expire(now, lim_.window_ms);
        if (history_posts_.sum + 1 > lim_.history_posts) return refuse(Verdict::HistoryPosts);
        if (history_bytes_.sum + bytes > lim_.history_bytes) return refuse(Verdict::HistoryBytes);
        history_posts_.add(now, 1);
        history_bytes_.add(now, bytes);
        return Verdict::Ok;
    }
    live_.expire(now, lim_.window_ms);
    if (live_.sum + 1 > lim_.live_posts) return refuse(Verdict::LiveTotal);
    if (!known_key) {
        new_keys_.expire(now, lim_.window_ms);
        if (new_keys_.sum + 1 > lim_.new_key_posts) return refuse(Verdict::NewKeys);
    }
    // Only keys that were admitted get an entry, so there are never more
    // entries than live posts in the window.
    auto it = keys_.find(p.author);
    if (it != keys_.end()) {
        it->second.topics.expire(now, lim_.window_ms);
        it->second.replies.expire(now, lim_.window_ms);
        if (p.kind == Kind::Topic && it->second.topics.sum + 1 > lim_.topics_per_key) return refuse(Verdict::KeyTopics);
        if (p.kind == Kind::Reply && it->second.replies.sum + 1 > lim_.replies_per_key) return refuse(Verdict::KeyReplies);
    } else {
        if ((p.kind == Kind::Topic ? lim_.topics_per_key : lim_.replies_per_key) == 0)
            return refuse(p.kind == Kind::Topic ? Verdict::KeyTopics : Verdict::KeyReplies);
        if (keys_.size() >= lim_.live_posts) expire_keys(now);
        it = keys_.emplace(p.author, KeyWindows{}).first;
    }
    (p.kind == Kind::Topic ? it->second.topics : it->second.replies).add(now, 1);
    live_.add(now, 1);
    if (!known_key) new_keys_.add(now, 1);
    return Verdict::Ok;
}

} // namespace forum
