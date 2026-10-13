// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/keystore.h"

#include <sodium.h>

#include <cstring>

namespace forum {
namespace {

constexpr uint8_t kVersion = 1;
constexpr size_t kSaltBytes = crypto_pwhash_SALTBYTES;                       // 16
constexpr size_t kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;  // 24
constexpr size_t kTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;       // 16
constexpr size_t kKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;     // 32
constexpr size_t kSealedKeyBytes = 1 + kNonceBytes + 64 + kTagBytes;
constexpr size_t kKdfBytes = 1 + 1 + 8 + 8 + kSaltBytes;
constexpr char kKeyAd[] = "logos-forum:key:v1";
constexpr char kCheckAd[] = "logos-forum:check:v1";
// What the check value seals: any constant does, its tag is what is checked.
constexpr uint8_t kCheckPlain[32] = {'l', 'o', 'g', 'o', 's', '-', 'f', 'o', 'r', 'u', 'm', ' ', 'k', 'e', 'y', 's'};

// Bounds on what a store may ask for, so a damaged or hostile file cannot
// make unlocking weaker than libsodium's minimum, or exhaust the machine.
constexpr uint64_t kMaxOps = 64;
constexpr uint64_t kMaxMem = 4ull << 30;

void put_u64(SecretBytes& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

struct Kdf {
    KdfParams params;
    uint8_t salt[kSaltBytes] = {};
};

std::optional<Kdf> read_kdf(const Store& s) {
    const auto v = s.meta("kdf");
    if (!v || v->size() != kKdfBytes || (*v)[0] != kVersion || (*v)[1] != crypto_pwhash_ALG_ARGON2ID13) return std::nullopt;
    Kdf k;
    k.params.ops = get_u64(v->data() + 2);
    k.params.mem = get_u64(v->data() + 10);
    std::memcpy(k.salt, v->data() + 18, kSaltBytes);
    if (k.params.ops < crypto_pwhash_OPSLIMIT_MIN || k.params.ops > kMaxOps) return std::nullopt;
    if (k.params.mem < crypto_pwhash_MEMLIMIT_MIN || k.params.mem > kMaxMem) return std::nullopt;
    return k;
}

SecretBytes write_kdf(const Kdf& k) {
    SecretBytes v{kVersion, static_cast<uint8_t>(crypto_pwhash_ALG_ARGON2ID13)};
    put_u64(v, k.params.ops);
    put_u64(v, k.params.mem);
    v.insert(v.end(), k.salt, k.salt + kSaltBytes);
    return v;
}

// The derived key, in guarded memory (sodium_malloc), or null.
uint8_t* derive(const std::string& passphrase, const Kdf& k) {
    auto* key = static_cast<uint8_t*>(sodium_malloc(kKeyBytes));
    if (!key) return nullptr;
    if (crypto_pwhash(key, kKeyBytes, passphrase.data(), passphrase.size(), k.salt, k.params.ops,
                      static_cast<size_t>(k.params.mem), crypto_pwhash_ALG_ARGON2ID13) != 0) {
        sodium_free(key);  // out of memory; sodium_free wipes
        return nullptr;
    }
    return key;
}

SecretBytes seal_bytes(const uint8_t* key, const uint8_t* plain, size_t n, const uint8_t* ad, size_t ad_len) {
    SecretBytes out(1 + kNonceBytes + n + kTagBytes);
    out[0] = kVersion;
    randombytes_buf(out.data() + 1, kNonceBytes);
    unsigned long long len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + 1 + kNonceBytes, &len, plain, n, ad, ad_len, nullptr,
                                               out.data() + 1, key);
    return out;
}

bool open_bytes(const uint8_t* key, const SecretBytes& sealed, uint8_t* plain, size_t n, const uint8_t* ad,
                size_t ad_len) {
    if (sealed.size() != 1 + kNonceBytes + n + kTagBytes || sealed[0] != kVersion) return false;
    unsigned long long len = 0;
    return crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &len, nullptr, sealed.data() + 1 + kNonceBytes,
                                                      sealed.size() - 1 - kNonceBytes, ad, ad_len, sealed.data() + 1,
                                                      key) == 0 && len == n;
}

SecretBytes key_ad(const PublicKey& pk) {
    SecretBytes ad(kKeyAd, kKeyAd + sizeof kKeyAd - 1);
    ad.insert(ad.end(), pk.begin(), pk.end());
    return ad;
}

bool check_value_opens(const Store& s, const uint8_t* key) {
    const auto c = s.meta("check");
    if (!c) return false;
    uint8_t plain[sizeof kCheckPlain];
    const bool ok = open_bytes(key, *c, plain, sizeof plain, reinterpret_cast<const uint8_t*>(kCheckAd), sizeof kCheckAd - 1);
    sodium_memzero(plain, sizeof plain);
    return ok;
}

// A secret key whose public half is the account's: anything else is damage.
bool matches(const SecretKey& sk, const PublicKey& pk) {
    PublicKey derived{};
    crypto_sign_ed25519_sk_to_pk(derived.data(), sk.data());
    return sodium_memcmp(derived.data(), pk.data(), pk.size()) == 0;
}

} // namespace

KdfParams KdfParams::desktop() { return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE}; }
KdfParams KdfParams::fastest() { return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN}; }

Keystore::Keystore(Store& store) : store_(store) {}
Keystore::~Keystore() { lock(); }

bool Keystore::is_protected() const { return store_.meta("kdf").has_value(); }

void Keystore::lock() {
    if (key_) sodium_free(key_);  // wipes before freeing
    key_ = nullptr;
}

Unlock Keystore::unlock(const std::string& passphrase) {
    if (!is_protected()) return Unlock::NotProtected;
    const auto kdf = read_kdf(store_);
    if (!kdf || !store_.meta("check")) return Unlock::Damaged;
    uint8_t* key = derive(passphrase, *kdf);
    if (!key) return Unlock::Damaged;
    if (!check_value_opens(store_, key)) {
        sodium_free(key);
        return Unlock::WrongPassphrase;
    }
    lock();
    key_ = key;
    return Unlock::Ok;
}

bool Keystore::check(const std::string& passphrase) const {
    const auto kdf = read_kdf(store_);
    if (!kdf) return false;
    uint8_t* key = derive(passphrase, *kdf);
    if (!key) return false;
    const bool ok = check_value_opens(store_, key);
    sodium_free(key);
    return ok;
}

bool Keystore::seal(const Account& a, SecretBytes& out) const {
    if (!key_) return false;
    const SecretBytes ad = key_ad(a.key.pk);
    out = seal_bytes(key_, a.key.sk.data(), a.key.sk.size(), ad.data(), ad.size());
    return true;
}

bool Keystore::open(const Store::AccountRow& r, SecretKey& out) const {
    if (!key_ || r.secret.size() != kSealedKeyBytes) return false;
    const SecretBytes ad = key_ad(r.pk);
    if (!open_bytes(key_, r.secret, out.data(), out.size(), ad.data(), ad.size()) || !matches(out, r.pk)) {
        sodium_memzero(out.data(), out.size());
        return false;
    }
    return true;
}

std::vector<Account> Keystore::accounts() const {
    const bool sealed = is_protected();
    std::vector<Account> out;
    for (const auto& r : store_.account_rows()) {
        Account a;
        a.label = r.label;
        a.key.pk = r.pk;
        a.created_ms = r.created_ms;
        a.posts = r.posts;
        a.backed_up = r.backed_up;
        if (r.secret.size() == a.key.sk.size()) {
            // In plain text: the normal case without a passphrase.
            std::memcpy(a.key.sk.data(), r.secret.data(), a.key.sk.size());
            if (!matches(a.key.sk, a.key.pk)) sodium_memzero(a.key.sk.data(), a.key.sk.size());
        } else if (sealed) {
            open(r, a.key.sk);  // stays zero if locked or damaged
        }
        out.push_back(std::move(a));
    }
    return out;
}

bool Keystore::save(const Account& a, bool selected) {
    if (!is_protected()) return store_.save_account(a, selected);
    if (!key_) return store_.update_account_info(a, selected);
    Store::AccountRow r;
    r.label = a.label;
    r.pk = a.key.pk;
    if (!matches(a.key.sk, a.key.pk) || !seal(a, r.secret)) return false;
    r.created_ms = a.created_ms;
    r.posts = a.posts;
    r.selected = selected;
    r.backed_up = a.backed_up;
    return store_.put_account_row(r);
}

bool Keystore::protect(const std::string& passphrase, KdfParams params) {
    if (passphrase.empty()) return false;
    if (is_protected() && !key_) return false;
    if (params.ops < crypto_pwhash_OPSLIMIT_MIN || params.ops > kMaxOps || params.mem < crypto_pwhash_MEMLIMIT_MIN ||
        params.mem > kMaxMem)
        return false;
    // Every key must be readable now, or it would be lost in the rewrite.
    const auto rows = store_.account_rows();
    std::vector<Account> all = accounts();
    for (const auto& a : all)
        if (!matches(a.key.sk, a.key.pk)) return false;

    Kdf kdf;
    kdf.params = params;
    randombytes_buf(kdf.salt, sizeof kdf.salt);
    uint8_t* key = derive(passphrase, kdf);
    if (!key) return false;
    uint8_t* previous = key_;
    key_ = key;
    bool ok = true;
    {
        Store::Txn txn(store_);
        ok = store_.set_meta("kdf", write_kdf(kdf)) &&
             store_.set_meta("check", seal_bytes(key_, kCheckPlain, sizeof kCheckPlain,
                                                 reinterpret_cast<const uint8_t*>(kCheckAd), sizeof kCheckAd - 1));
        for (size_t i = 0; ok && i < all.size(); ++i) {
            Store::AccountRow r;
            r.label = all[i].label;
            r.pk = all[i].key.pk;
            r.created_ms = all[i].created_ms;
            r.posts = all[i].posts;
            r.selected = rows[i].selected;
            r.backed_up = all[i].backed_up;
            ok = seal(all[i], r.secret) && store_.put_account_row(r);
        }
        ok = ok && txn.commit();
    }
    if (!ok) {
        sodium_free(key_);
        key_ = previous;
        return false;
    }
    if (previous) sodium_free(previous);
    store_.compact();  // the plain (or previously sealed) keys leave the file
    return true;
}

bool Keystore::unprotect(const std::string& passphrase) {
    if (!is_protected() || !check(passphrase)) return false;
    if (!key_ && unlock(passphrase) != Unlock::Ok) return false;
    const auto rows = store_.account_rows();
    std::vector<Account> all = accounts();
    for (const auto& a : all)
        if (!matches(a.key.sk, a.key.pk)) return false;  // a key that does not open would be lost
    bool ok = true;
    {
        Store::Txn txn(store_);
        ok = store_.erase_meta("kdf") && store_.erase_meta("check");
        for (size_t i = 0; ok && i < all.size(); ++i) ok = store_.save_account(all[i], rows[i].selected);
        ok = ok && txn.commit();
    }
    if (!ok) return false;
    lock();
    store_.compact();
    return true;
}

} // namespace forum
