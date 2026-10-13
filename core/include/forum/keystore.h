// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Account keys at rest.
//
// Without a passphrase, keys are kept as before: in forum.db, which only its
// owner can read (0600, in a 0700 directory). With one, every account's
// secret key is sealed with XChaCha20-Poly1305 under a key derived from the
// passphrase by Argon2id (crypto_pwhash), and the keys are unlocked once per
// session. The forum reads and posts anonymously while locked; posting as an
// account needs the keys unlocked.
//
// On disk: a random salt and the Argon2id limits ("kdf"), a sealed check
// value ("check") that tells a wrong passphrase from a damaged file, and per
// account a blob 0x01 | nonce (24) | sealed secret key (64 + 16), bound to
// the account's public key so blobs cannot be swapped between accounts.
// The secret of every private forum joined (forum_groups.secret) is sealed
// the same way, bound to the forum's id.
//
// Turning a passphrase on seals keys that were in plain text and rewrites the
// file (secure_delete, VACUUM, WAL checkpoint), so the plain copies are gone
// from forum.db and its log. A copy of the file taken before still has them.
#pragma once

#include "forum/identity.h"
#include "forum/store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forum {

// Argon2id work. desktop(): libsodium's MODERATE limits (3 passes, 256 MiB),
// about a second on a laptop, paid once per session.
struct KdfParams {
    uint64_t ops = 0;
    uint64_t mem = 0;
    static KdfParams desktop();
    static KdfParams fastest();  // libsodium's minimum: for tests only
};

enum class Unlock { Ok, WrongPassphrase, NotProtected, Damaged };

class Keystore {
public:
    explicit Keystore(Store& store);
    ~Keystore();  // locks: the derived key is wiped
    Keystore(const Keystore&) = delete;
    Keystore& operator=(const Keystore&) = delete;

    bool is_protected() const;
    bool is_unlocked() const { return key_ != nullptr; }

    // Derive the key and check it. A wrong passphrase changes nothing.
    Unlock unlock(const std::string& passphrase);
    // Forget the derived key (wiped). Keys already handed out are the
    // caller's to drop.
    void lock();
    // Whether `passphrase` is the one in use, without unlocking.
    bool check(const std::string& passphrase) const;

    // Seal every key under a new passphrase. From plain text (the migration),
    // or, when unlocked, from the current passphrase. Empty passphrases are
    // refused. Leaves the store unlocked under the new passphrase.
    bool protect(const std::string& passphrase, KdfParams params = KdfParams::desktop());
    // Back to plain text keys (needs the current passphrase).
    bool unprotect(const std::string& passphrase);

    // The accounts with their secret keys: zero while locked, and for any
    // sealed key that does not open (a damaged row).
    std::vector<Account> accounts() const;
    std::optional<std::string> selected_label() const { return store_.selected_label(); }

    // Write an account. While locked only what is not secret can change (post
    // count, selection), for an account that exists with this key.
    bool save(const Account& a, bool selected);

    // Other secrets kept in forum.db (a private forum's 32-byte secret),
    // sealed the same way and bound to `context` (what they belong to).
    // Without a passphrase both copy as is; locked, a sealed one fails.
    bool seal_secret(const std::string& context, const uint8_t* plain, size_t n, SecretBytes& out) const;
    bool open_secret(const std::string& context, const SecretBytes& stored, uint8_t* plain, size_t n) const;

private:
    Store& store_;
    uint8_t* key_ = nullptr;  // sodium_malloc'd, 32 bytes, guarded; null when locked
    bool seal(const Account& a, SecretBytes& out) const;
    bool open(const Store::AccountRow& r, SecretKey& out) const;
};

} // namespace forum
