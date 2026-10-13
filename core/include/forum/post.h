// SPDX-License-Identifier: MIT OR Apache-2.0
//
// A forum post on the wire: what it says, who signed it, and how.
//
// Every post is signed with Ed25519 and VERIFIED on receipt — a post whose
// signature does not check against the key it names is dropped, so an author
// line in this forum is a fact, not a claim. The post id is the SHA-256 of the
// signed bytes, which makes a post tamper-evident and lets any copy of it,
// from any peer or from history, be de-duplicated by id alone.
//
// Three ways to sign, chosen per post:
//   Identity  — an account's long-lived key; posts link to each other.
//   Alias     — the same kind of key, shown under a chosen name.
//   Anonymous — a key minted for this one post and then discarded, so two
//               anonymous posts cannot be linked to each other or to anyone.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forum {

using PublicKey = std::array<uint8_t, 32>;
using SecretKey = std::array<uint8_t, 64>;
using Signature = std::array<uint8_t, 64>;

enum class Kind { Topic, Reply };
enum class Mode { Identity, Alias, Anonymous };

struct Post {
    int version = 1;
    Kind kind = Kind::Topic;
    std::string forum;      // e.g. "Logos Forum"
    std::string topic_id;   // replies only: the id of the topic post
    std::string title;      // topics only
    std::string body;
    Mode mode = Mode::Identity;
    std::string alias;      // Alias mode only: the display name
    uint64_t ts_ms = 0;     // the author's clock; display and ordering only
    PublicKey author{};     // the key the signature checks against
    Signature sig{};
    // Not signed itself: the image manifest the body's last line commits to
    // (see media.h). Empty for a post without images, or when a peer that
    // does not keep it relayed the post.
    std::string att;

    // The exact bytes that are signed and hashed: every field above except
    // `sig`, length-prefixed so no two different posts serialise the same way.
    std::vector<uint8_t> signing_bytes() const;

    // hex(SHA-256(signing_bytes)) — stable, content-derived, tamper-evident.
    std::string id() const;
};

// Why a post was refused. Anything other than Ok means the post is dropped.
enum class Check { Ok, Malformed, BadSignature, MissingTitle, MissingTopic, TooLong };

// Limits that keep one post from flooding peers or local storage.
constexpr size_t kMaxTitle = 200;
constexpr size_t kMaxBody = 20000;
constexpr size_t kMaxAlias = 40;
constexpr size_t kMaxForum = 80;

// Sign `p` in place with `sk` (whose public half must be `p.author`).
void sign(Post& p, const SecretKey& sk);

// Structural rules plus the signature.
Check verify(const Post& p);

// Compact JSON for Logos Delivery; binary fields are base64. "att" is
// written only when present, so a post without images encodes exactly as in
// 0.3.x.
std::string encode(const Post& p);

// Parse and VERIFY. Returns nothing for anything that is not a well-formed,
// correctly signed post — non-forum traffic on the topic is ignored this way.
// An "att" that is not the manifest the signed body commits to is dropped
// (the post itself is kept: its signature holds).
std::optional<Post> decode(const std::string& json, Check* why = nullptr);

std::string to_hex(const uint8_t* data, size_t len);

// An alias as it may be shown next to other text: without the characters that
// would reorder or hide what follows it (bidi controls, zero-width and other
// invisible format characters, C0/C1 controls, line and paragraph breaks),
// with runs of whitespace as one space, trimmed, and without bytes that are
// not UTF-8. Empty if nothing visible is left. Display only: the signed post
// keeps the alias as its author wrote it.
std::string display_alias(const std::string& alias);

} // namespace forum
