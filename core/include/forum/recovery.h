// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Backing up an account as 24 words.
//
// An account key is an Ed25519 key, and all of it follows from its 32-byte
// seed. The phrase is that seed written with the BIP-39 English word list:
// 256 bits and an 8-bit checksum (the first byte of SHA-256 of the seed), 24
// words of 11 bits. A mistyped or swapped word fails the checksum (all but 1
// in 256 do), a word not in the list is named, and restoring gives back the
// same public key. Only the word encoding is BIP-39's: the seed is used as is,
// not stretched into a wallet seed, so these words are not a wallet's.
//
// What a phrase does not restore, by design:
//   - anonymous keys: each is random, made for one post and wiped, and is
//     never derived from an account; nothing restores or repeats one;
//   - retired keys: rotation draws a fresh random key, not derived from the
//     old one. A phrase restores the key it was written for. After a rotation
//     the account is marked as not backed up until a new phrase is shown.
#pragma once

#include "forum/identity.h"

#include <optional>
#include <string>

namespace forum {

constexpr size_t kRecoveryWords = 24;

// The 24 words for an account key, separated by single spaces. The caller
// wipes the string once shown.
std::string recovery_phrase(const SecretKey& sk);

// The key back from its words: case and runs of whitespace do not matter.
// On failure `error` says why (a word not in the list, by position; the wrong
// number of words; a checksum that does not match).
std::optional<Keypair> restore_from_phrase(const std::string& phrase, std::string* error = nullptr);

// The word list itself (2048 words), for tests and for suggestions.
const char* recovery_word(size_t i);

} // namespace forum
