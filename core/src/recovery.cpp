// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/recovery.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace forum {
namespace {

const char* const kWords[2048] = {
#include "bip39_english.inc"
};

int word_index(const std::string& w) {
    // The list is sorted: a binary search.
    const auto* end = kWords + 2048;
    const auto* it = std::lower_bound(kWords, end, w, [](const char* a, const std::string& b) { return std::strcmp(a, b.c_str()) < 0; });
    return it != end && w == *it ? static_cast<int>(it - kWords) : -1;
}

} // namespace

const char* recovery_word(size_t i) { return i < 2048 ? kWords[i] : ""; }

std::string recovery_phrase(const SecretKey& sk) {
    // 32 bytes of seed, then one byte of checksum: 264 bits, 24 x 11.
    uint8_t bits[33];
    crypto_sign_ed25519_sk_to_seed(bits, sk.data());
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, bits, 32);
    bits[32] = h[0];
    std::string out;
    for (size_t w = 0; w < kRecoveryWords; ++w) {
        unsigned v = 0;
        for (size_t b = w * 11; b < w * 11 + 11; ++b) v = (v << 1) | ((bits[b / 8] >> (7 - b % 8)) & 1);
        if (w) out += ' ';
        out += kWords[v];
    }
    sodium_memzero(bits, sizeof bits);
    sodium_memzero(h, sizeof h);
    return out;
}

std::optional<Keypair> restore_from_phrase(const std::string& phrase, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<Keypair> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    uint8_t bits[33] = {};
    size_t n = 0;
    std::string word;
    bool bad = false;
    std::string why;
    auto flush = [&]() {
        if (word.empty()) return;
        ++n;
        const int v = n <= kRecoveryWords ? word_index(word) : 0;
        if (v < 0 && !bad) {
            bad = true;
            why = "word " + std::to_string(n) + " is not in the word list";
        }
        if (v > 0)
            for (size_t k = 0; k < 11; ++k)
                if ((v >> (10 - k)) & 1) {
                    const size_t b = (n - 1) * 11 + k;
                    bits[b / 8] |= static_cast<uint8_t>(1u << (7 - b % 8));
                }
        sodium_memzero(word.data(), word.size());
        word.clear();
    };
    for (const char c : phrase) {
        const auto u = static_cast<unsigned char>(c);
        if (u == ' ' || u == '\t' || u == '\n' || u == '\r') flush();
        else word.push_back(static_cast<char>(u >= 'A' && u <= 'Z' ? u - 'A' + 'a' : u));
    }
    flush();
    if (n != kRecoveryWords) {
        sodium_memzero(bits, sizeof bits);
        return fail("a recovery phrase has 24 words; this one has " + std::to_string(n));
    }
    if (bad) {
        sodium_memzero(bits, sizeof bits);
        return fail(why);
    }
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, bits, 32);
    const bool sums = h[0] == bits[32];
    sodium_memzero(h, sizeof h);
    if (!sums) {
        sodium_memzero(bits, sizeof bits);
        return fail("the words do not check out: one is mistyped, or two are swapped");
    }
    Keypair k;
    crypto_sign_seed_keypair(k.pk.data(), k.sk.data(), bits);
    sodium_memzero(bits, sizeof bits);
    return k;
}

} // namespace forum
