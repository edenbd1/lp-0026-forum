// SPDX-License-Identifier: MIT OR Apache-2.0
//
// What a reader chooses not to see. Local, and per account.
//
// Muting is by public key, the only thing a post proves about its author: a
// display name (an alias) is anyone's to type, so muting by name would hide
// whoever copies it, and miss the author under another one. An anonymous
// post's key is used once, so muting it hides that one post.
//
// Hiding is by post id: a topic (with its thread) or a single reply.
//
// None of this is sent, shared or put in a snapshot or a history answer, and
// it changes nothing the node stores or serves to others: a muted author's
// posts are still kept and still passed on in history, so muting decides only
// what this account's reader is shown. It lives in forum.db, beside the
// account it belongs to, and goes when that account is deleted.
#pragma once

#include "forum/post.h"
#include "forum/store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace forum {

struct Muted {
    PublicKey key{};
    uint64_t at_ms = 0;
};
struct Hidden {
    std::string id;
    uint64_t at_ms = 0;
};

class Moderation {
public:
    explicit Moderation(Store& store);

    // Each returns false when there was nothing to change (or the id is not a
    // post id: 64 lowercase hex characters).
    bool mute(const std::string& account, const PublicKey& key, uint64_t now_ms);
    bool unmute(const std::string& account, const PublicKey& key);
    bool is_muted(const std::string& account, const PublicKey& key) const;
    std::vector<Muted> muted(const std::string& account) const;   // newest first

    bool hide(const std::string& account, const std::string& post_id, uint64_t now_ms);
    bool unhide(const std::string& account, const std::string& post_id);
    bool is_hidden(const std::string& account, const std::string& post_id) const;
    std::vector<Hidden> hidden(const std::string& account) const;  // newest first

    // What `account` is shown: topics without the hidden ones and those by a
    // muted key; replies without the hidden ones and those by a muted key.
    std::vector<TopicRow> visible(const std::string& account, std::vector<TopicRow> topics) const;
    std::vector<Post> visible(const std::string& account, std::vector<Post> replies) const;

    // The account is gone: so is what it muted and hid.
    void forget(const std::string& account);

private:
    Store& store_;
};

} // namespace forum
