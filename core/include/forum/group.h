// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Private group forums, joined from an invite.
//
// A group is a name and a random 32-byte secret, and the invite carries both:
// logosforum://join/<base64url(1 | secret | name)>. Whoever holds the invite is
// a member. From the secret, three independent subkeys are derived with
// libsodium's crypto_kdf (BLAKE2b, context "LFgroup1"):
//
//   1  the group's content topic on Logos Delivery, /logos-forum/1/g-<hex>/sealed
//   2  the key that seals every message (XChaCha20-Poly1305, AEAD)
//   3  the forum id signed into the group's posts, "g:<hex>"
//
// So knowing the topic (which the network sees) tells nothing about the key,
// and the group's name never leaves the invite.
//
// Inside, a group is an ordinary forum: the same signed posts, history
// requests and bundles, handled by the same engine. SealedTransport seals each
// of them before it reaches the network and opens what comes back, so
// everything on the group's topic is ciphertext of padded length. The public
// forum's topic and wire format do not change, and public forum nodes never
// subscribe to a group's topic.
//
// Not protected: a member can pass on what they read or the invite itself;
// the network sees that messages go to a group topic, when, from which relay
// peer and of what padded size; and nobody can be removed, because the secret
// cannot be revoked: the way out is a new group with a new invite.
#pragma once

#include "forum/engine.h"
#include "forum/keystore.h"
#include "forum/store.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forum {

using GroupSecret = std::array<uint8_t, 32>;
using SealKey = std::array<uint8_t, 32>;

constexpr const char* kInvitePrefix = "logosforum://join/";
constexpr size_t kMaxGroupName = 64;
// What a sealed message may weigh on the wire before it is even decoded.
constexpr size_t kMaxSealedBytes = 220 * 1024;

struct Group {
    std::string name;   // as shown: plain text, cleaned like an alias
    GroupSecret secret{};

    // A new group with a fresh random secret. Empty name: nothing.
    static std::optional<Group> create(const std::string& name);
    // Parse an invite. Anything else (another prefix, bad base64, a wrong
    // version or length, an empty or oversized name) gives nothing.
    static std::optional<Group> from_invite(const std::string& invite);
    std::string invite() const;

    std::string forum_id() const;       // "g:" + 32 hex, signed into each post
    std::string content_topic() const;  // where its sealed messages travel
    SealKey seal_key() const;
};

// Seal `plain` for the group's topic: base64(1 | nonce | AEAD(pad(plain))),
// the topic bound in as associated data. Padded to a multiple of 1 KiB, so
// the size on the wire says little about the post.
std::string seal(const SealKey& key, const std::string& topic, const std::string& plain);
// Open it again; nothing for anything that is not exactly that, under that key
// and topic: another key, a changed byte, a truncation, a replay to another
// group's topic.
std::optional<std::string> open_sealed(const SealKey& key, const std::string& topic, const std::string& wire);

// The engine's transport for a group: every message it sends is sealed and
// sent on the group's topic; history read from the network is opened, and
// what does not open is dropped. The topic the engine names is ignored.
class SealedTransport : public Transport {
public:
    SealedTransport(Transport& inner, const Group& g)
        : inner_(inner), topic_(g.content_topic()), key_(g.seal_key()) {}
    ~SealedTransport() override;
    SendResult send(const std::string& content_topic, const std::string& payload) override;
    bool subscribe(const std::string& content_topic) override;
    std::vector<std::string> history(const std::string& content_topic) override;
    std::optional<std::string> open(const std::string& wire) const { return open_sealed(key_, topic_, wire); }
    const std::string& topic() const { return topic_; }

private:
    Transport& inner_;
    std::string topic_;
    SealKey key_;
};

// The groups this install has joined, in forum.db. With `keys` sealed by a
// passphrase, each group's secret is stored sealed (keystore.h): locked, no
// group opens and none can be joined.
class Groups {
public:
    explicit Groups(Store& store, const Keystore* keys = nullptr);
    bool save(const Group& g, uint64_t now_ms);   // false if already joined
    std::vector<Group> all() const;               // in the order joined
    // Leave: the group, its posts and anything of it still in the outbox go
    // from this device (secure_delete overwrites them; the WAL is flushed).
    bool remove(const std::string& forum_id);

private:
    Store& store_;
    const Keystore* keys_ = nullptr;
};

} // namespace forum
