// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Signed reactions: 👍 ❤️ 🎉 👀 ✅ under a post.
//
// A reaction is its own message on the forum's topic, signed like a post by
// the reacting account's key: forum, target post id, emoji, on or off, date
// and key. It is not a post: a 0.3.x peer's decode() refuses it (it has no
// "v", "kind" or "body"), its receive() then finds none of the history tags
// it knows, and drops it after the one JSON parse every message costs.
//
// On this version a reaction is kept once per (key, post, emoji): the newest
// signed state wins, so "unreact" is a newer signed message with on = false.
// What arrives is limited before any signature is checked (a budget for the
// whole topic), then per key once the signature holds, both on arrival time.
#pragma once

#include "forum/engine.h"
#include "forum/post.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace forum {

class Store;

constexpr int kEmojiCount = 5;
// The fixed set, by code: 0 👍, 1 ❤️, 2 🎉, 3 👀, 4 ✅.
extern const char* const kEmoji[kEmojiCount];

struct Reaction {
    std::string forum;
    std::string target;   // the id of the post reacted to
    int emoji = 0;        // 0 … kEmojiCount-1
    bool on = true;       // false: the reaction is withdrawn
    uint64_t ts_ms = 0;   // the author's clock: the newest state wins
    PublicKey author{};
    Signature sig{};

    std::vector<uint8_t> signing_bytes() const;
};

void sign(Reaction& r, const SecretKey& sk);
// Structure only (no signature): cheap, checked before anything else.
bool well_formed(const Reaction& r);
bool verify(const Reaction& r);
std::string encode(const Reaction& r);
// Parse only; verify() separately. Nothing for anything that is not a
// reaction's shape.
std::optional<Reaction> decode_reaction(const std::string& json);

// Kept reactions and the limits on what arrives.
class Reactions {
public:
    explicit Reactions(Store& store);

    // Every reaction from the network goes through here: live ones under the
    // arrival limits; ones carried in a history bundle without them (their
    // number is capped per bundle by the caller, kMaxPerBundle, and costs what
    // the bundle's posts cost).
    enum class Outcome { Stored, Unchanged, Malformed, OverBudget, BadSignature, OverKeyLimit, UnknownPost };
    Outcome receive(const Reaction& r, uint64_t now_ms, bool live = true);

    // Ours: kept at once, sent by the caller.
    bool put(const Reaction& r);
    // The newest state we hold for (key, post, emoji), if any.
    std::optional<Reaction> get(const PublicKey& author, const std::string& target, int emoji) const;

    // For each emoji, how many keys have it on for `target`.
    std::array<int, kEmojiCount> counts(const std::string& target) const;
    // Reactions on the given posts (on and off), for history bundles.
    std::vector<Reaction> on_posts(const std::vector<std::string>& ids, size_t limit) const;

    // Limits. Before any signature is checked: a bucket for the whole topic.
    static constexpr double kTopicBurst = 60, kTopicPerMinute = 120;
    // Once the signature holds: per key.
    static constexpr double kKeyBurst = 10, kKeyPerMinute = 20;
    static constexpr size_t kMaxTrackedKeys = 4096;
    static constexpr size_t kMaxPerBundle = 500;
    uint64_t verified() const { return verified_; }    // signatures checked
    uint64_t dropped() const { return dropped_; }
    size_t tracked_keys() const { return keys_.size(); }

private:
    sqlite3* db_;
    Store& store_;
    RateLimiter topic_{kTopicBurst, kTopicPerMinute};
    struct KeyBucket { RateLimiter bucket{kKeyBurst, kKeyPerMinute}; uint64_t last_ms = 0; };
    std::map<PublicKey, KeyBucket> keys_;
    uint64_t verified_ = 0, dropped_ = 0;
};

} // namespace forum
