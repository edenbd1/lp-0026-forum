// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Replies to you, and your posts, for one account at a time.
//
// "You" is a public key: the selected account's current key. Never a name: an
// alias, an account label or "anonymous" can be written by anyone, a key's
// signature cannot. So
//   - a reply is yours to read when it answers a topic your key started, or
//     follows a reply your key wrote in a topic (replies are flat);
//   - a mention is "@" and at least the first 16 hex digits of your key
//     (mention_handle); "@Alice" mentions nobody;
//   - anonymous posts are never yours here: each is signed by a one-time key
//     that is wiped, and the forum keeps no note of which anonymous posts
//     this install wrote, so nothing on disk links them to an account. The
//     cost: replies to your anonymous posts do not reach your inbox.
//   - keys an account rotated away from are not remembered (that would link
//     them to the new one on disk), so replies to posts made under a retired
//     key no longer reach the inbox either.
// Another account on the same install has its own key, so its own inbox and
// its own posts: neither view of one account ever shows the other's.
#pragma once

#include "forum/store.h"

#include <set>
#include <string>
#include <vector>

namespace forum {

// The 16 lowercase hex digits that, after "@", mention `pk`.
constexpr size_t kMentionDigits = 16;
std::string mention_handle(const PublicKey& pk);

// Whether `text` mentions `pk`: an "@" followed by a run of 16 to 64 ASCII
// hex digits (any case, ending at a non-hex character) that is a prefix of
// the key's hex. A longer run that only starts like the key is someone else.
bool mentions(const std::string& text, const PublicKey& pk);

enum class Why { YourTopic, AfterYourReply, Mention };

struct InboxItem {
    Stored post;
    Why why = Why::YourTopic;
    bool unread = false;
};

struct InboxView {
    std::vector<InboxItem> items;  // newest received first
    size_t unread = 0;
};

// What reached `me` in a forum: posts by other keys, received after
// `read_upto` counted as unread. `skip` holds post ids not to list (the
// embedder's anonymous posts of this session, kept in memory only).
InboxView inbox(const Store& store, const std::string& forum, const PublicKey& me, uint64_t read_upto,
                const std::set<std::string>& skip = {}, size_t limit = 200);

// Posts signed by `me` (as the account or under an alias), newest first.
std::vector<Stored> my_posts(const Store& store, const std::string& forum, const PublicKey& me, size_t limit = 200);

} // namespace forum
