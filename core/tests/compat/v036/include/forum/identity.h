// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Who a post is signed as.
//
// An account is a long-lived Ed25519 key with a local label; posting as it (or
// under an alias on it) makes those posts linkable, which is the point of an
// identity. Anonymous posting never touches an account: each anonymous post is
// signed by a key generated for that post alone and wiped right after, so the
// network sees an unrelated, valid signer every time.
//
// Rotation answers the privacy cost of a long-lived identity: an account can be
// retired in favour of a fresh key after a number of posts or a period of time,
// and the old key simply stops being used. Nothing links the two keys unless the
// user chooses to say so.
#pragma once

#include "forum/post.h"

#include <cstdint>
#include <string>
#include <vector>

namespace forum {

struct Keypair {
    PublicKey pk{};
    SecretKey sk{};
    static Keypair generate();
};

struct Account {
    std::string label;
    Keypair key;
    uint64_t created_ms = 0;
    uint32_t posts = 0;  // posts signed since this key was created
};

// When an account's key is replaced by a fresh one. Zero disables a limit.
struct RotationPolicy {
    uint32_t max_posts = 0;
    uint64_t max_age_ms = 0;
    bool due(const Account& a, uint64_t now_ms) const;
};

// Build and sign a post as `a` (Identity, or Alias when `alias` is non-empty),
// counting it against the account.
Post author_as(Account& a, Post p, const std::string& alias = {});

// Build and sign a post under a one-time key that is wiped before returning.
Post author_anonymously(Post p);

// Replace `a`'s key with a fresh one, keeping its label. Returns the retired
// public key so a caller can show "you were …" if the user wants to.
PublicKey rotate(Account& a, uint64_t now_ms);

// Short, readable rendering of a key for display: first 4 and last 4 bytes.
std::string short_key(const PublicKey& pk);

} // namespace forum
