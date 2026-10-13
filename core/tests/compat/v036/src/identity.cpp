// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/identity.h"

#include <sodium.h>

namespace forum {

Keypair Keypair::generate() {
    Keypair k;
    crypto_sign_keypair(k.pk.data(), k.sk.data());
    return k;
}

bool RotationPolicy::due(const Account& a, uint64_t now_ms) const {
    if (max_posts && a.posts >= max_posts) return true;
    if (max_age_ms && now_ms >= a.created_ms && now_ms - a.created_ms >= max_age_ms) return true;
    return false;
}

Post author_as(Account& a, Post p, const std::string& alias) {
    p.mode = alias.empty() ? Mode::Identity : Mode::Alias;
    p.alias = alias;
    p.author = a.key.pk;
    sign(p, a.key.sk);
    ++a.posts;
    return p;
}

Post author_anonymously(Post p) {
    Keypair once = Keypair::generate();
    p.mode = Mode::Anonymous;
    p.alias.clear();
    p.author = once.pk;
    sign(p, once.sk);
    sodium_memzero(once.sk.data(), once.sk.size());
    return p;
}

PublicKey rotate(Account& a, uint64_t now_ms) {
    const PublicKey old = a.key.pk;
    sodium_memzero(a.key.sk.data(), a.key.sk.size());
    a.key = Keypair::generate();
    a.created_ms = now_ms;
    a.posts = 0;
    return old;
}

std::string short_key(const PublicKey& pk) {
    return to_hex(pk.data(), 4) + "…" + to_hex(pk.data() + 28, 4);
}

} // namespace forum
