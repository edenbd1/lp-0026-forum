// SPDX-License-Identifier: MIT OR Apache-2.0
// Accounts: keys at rest, backup and restore, replies to you. Included by
// tests.cpp, whose TEST/CHECK, topic(), Bus and FakeNet these use.
#include "forum/keystore.h"
#include "forum/recovery.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <set>

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char d[] = "/tmp/forum-acct-XXXXXX";
        path = mkdtemp(d) ? d : "/tmp";
    }
    ~TempDir() {
        for (const char* f : {"/forum.db", "/forum.db-wal", "/forum.db-shm", "/forum.db-journal"})
            std::remove((path + f).c_str());
        rmdir(path.c_str());
    }
    std::string db() const { return path + "/forum.db"; }
};

std::string file_bytes(const std::string& f) {
    std::ifstream in(f, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
// Whether forum.db or its log or shared-memory file holds these bytes anywhere.
bool on_disk(const std::string& db, const uint8_t* p, size_t n) {
    const std::string needle(reinterpret_cast<const char*>(p), n);
    for (const char* side : {"", "-wal", "-shm", "-journal"})
        if (file_bytes(db + side).find(needle) != std::string::npos) return true;
    return false;
}
bool on_disk(const std::string& db, const std::string& s) {
    return on_disk(db, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
std::array<uint8_t, 32> seed_of(const SecretKey& sk) {
    std::array<uint8_t, 32> s{};
    crypto_sign_ed25519_sk_to_seed(s.data(), sk.data());
    return s;
}
bool zero(const SecretKey& sk) { return sodium_is_zero(sk.data(), sk.size()) == 1; }

// A forum.db as the forum wrote it before passphrases, backups and inboxes.
void write_legacy_store(const std::string& path, const std::vector<Account>& accounts) {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, R"sql(
PRAGMA journal_mode=WAL;
CREATE TABLE posts (
  id TEXT PRIMARY KEY, forum TEXT NOT NULL, kind INTEGER NOT NULL, topic_id TEXT NOT NULL,
  title TEXT NOT NULL, body TEXT NOT NULL, mode INTEGER NOT NULL, alias TEXT NOT NULL,
  ts INTEGER NOT NULL, author BLOB NOT NULL, sig BLOB NOT NULL, received INTEGER NOT NULL);
CREATE TABLE outbox (
  id TEXT PRIMARY KEY, payload TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
  next_try INTEGER NOT NULL, last_error TEXT NOT NULL DEFAULT '', resends INTEGER NOT NULL DEFAULT 0);
CREATE TABLE accounts (
  label TEXT PRIMARY KEY, pk BLOB NOT NULL, sk BLOB NOT NULL, created INTEGER NOT NULL,
  posts INTEGER NOT NULL, selected INTEGER NOT NULL DEFAULT 0);
PRAGMA user_version=1;
)sql", nullptr, nullptr, nullptr);
    for (size_t i = 0; i < accounts.size(); ++i) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO accounts VALUES (?,?,?,?,?,?)", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, accounts[i].label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 2, accounts[i].key.pk.data(), 32, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 3, accounts[i].key.sk.data(), 64, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, static_cast<int64_t>(accounts[i].created_ms));
        sqlite3_bind_int64(s, 5, accounts[i].posts);
        sqlite3_bind_int64(s, 6, i == 0 ? 1 : 0);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
}

Post reply_to(const std::string& topic_id, const std::string& body, uint64_t ts) {
    Post p; p.kind = Kind::Reply; p.forum = "Logos Forum"; p.topic_id = topic_id; p.body = body; p.ts_ms = ts;
    return p;
}

} // namespace

// ─── Keys at rest ─────────────────────────────────────────────────────────────

#ifndef _WIN32
TEST(without_a_passphrase_the_store_its_log_and_its_shared_memory_are_owner_only) {
    // Today's behaviour, kept: no passphrase, keys in forum.db, and every file
    // SQLite makes for it (the -wal the key is written to first, and -shm)
    // readable by its owner only, whatever the umask.
    TempDir dir;
    const mode_t old_mask = umask(0);  // the most permissive umask there is
    auto mode = [](const std::string& f) { struct stat st{}; return stat(f.c_str(), &st) == 0 ? int(st.st_mode & 0777) : -1; };
    Account me{"me", Keypair::generate(), 1};
    {
        Store s(dir.db());
        Keystore keys(s);
        CHECK(!keys.is_protected());
        CHECK(keys.save(me, true));
        CHECK(mode(dir.db()) == 0600 && mode(dir.db() + "-wal") == 0600 && mode(dir.db() + "-shm") == 0600);
        CHECK(keys.accounts().size() == 1 && keys.accounts()[0].key.sk == me.key.sk);   // usable as before
    }
    umask(old_mask);
}
#endif

TEST(a_passphrase_seals_existing_plain_keys_and_leaves_no_plain_copy_on_disk) {
    // The migration: a store written before passphrases, with plain keys.
    TempDir dir;
    std::vector<Account> before{{"Alice", Keypair::generate(), 10, 3}, {"Work", Keypair::generate(), 20, 1}};
    write_legacy_store(dir.db(), before);
    {
        Store s(dir.db());   // opens and upgrades the old schema
        Keystore keys(s);
        CHECK(!keys.is_protected());
        const auto plain = keys.accounts();
        CHECK(plain.size() == 2 && plain[0].key.sk == before[0].key.sk && plain[1].key.sk == before[1].key.sk);
        // The positive control: the plain seeds are in the file now, so the
        // scan below can find them.
        const auto seed0 = seed_of(before[0].key.sk);
        CHECK(on_disk(dir.db(), seed0.data(), seed0.size()));
        CHECK(keys.protect("correct horse battery staple", KdfParams::fastest()));
        CHECK(keys.is_protected() && keys.is_unlocked());
    }
    for (const auto& a : before) {
        const auto seed = seed_of(a.key.sk);
        CHECK(!on_disk(dir.db(), seed.data(), seed.size()));
        CHECK(!on_disk(dir.db(), a.key.sk.data(), 32));
    }
    // A new session starts locked: the keys are not readable without it.
    Store s(dir.db());
    Keystore keys(s);
    CHECK(keys.is_protected() && !keys.is_unlocked());
    for (const auto& a : keys.accounts()) CHECK(zero(a.key.sk));
    CHECK(keys.accounts()[0].label == "Alice" && keys.accounts()[0].posts == 3 && keys.selected_label() == std::string("Alice"));
    CHECK(keys.unlock("correct horse battery staple") == Unlock::Ok);
    const auto opened = keys.accounts();
    CHECK(opened.size() == 2 && opened[0].key.sk == before[0].key.sk && opened[1].key.sk == before[1].key.sk);
}

TEST(a_wrong_passphrase_fails_cleanly_and_changes_nothing) {
    TempDir dir;
    Account me{"me", Keypair::generate(), 1};
    {
        Store s(dir.db()); Keystore keys(s);
        keys.save(me, true);
        CHECK(!keys.protect("", KdfParams::fastest()));                 // empty: refused
        CHECK(!keys.is_protected());
        CHECK(keys.protect("right", KdfParams::fastest()));
    }
    Store s(dir.db()); Keystore keys(s);
    const auto rows_before = s.account_rows();
    const auto kdf_before = s.meta("kdf");
    for (const char* wrong : {"wrong", "Right", "right ", "", "righ"}) {
        CHECK(keys.unlock(wrong) == Unlock::WrongPassphrase);
        CHECK(!keys.is_unlocked() && !keys.check(wrong));
        CHECK(!keys.unprotect(wrong));
        CHECK(!keys.protect("new", KdfParams::fastest()));   // locked: cannot re-seal either
    }
    for (const auto& a : keys.accounts()) CHECK(zero(a.key.sk));
    const auto rows_after = s.account_rows();
    CHECK(rows_after.size() == 1 && rows_after[0].secret == rows_before[0].secret && s.meta("kdf") == kdf_before);
    CHECK(keys.check("right") && !keys.is_unlocked());     // checking does not unlock
    CHECK(keys.unlock("right") == Unlock::Ok && keys.accounts()[0].key.sk == me.key.sk);
}

TEST(a_plain_key_cannot_be_written_next_to_sealed_ones) {
    Store s(":memory:"); Keystore keys(s);
    Account me{"me", Keypair::generate(), 1};
    keys.save(me, true);
    CHECK(keys.protect("pass", KdfParams::fastest()));
    Account other{"other", Keypair::generate(), 2};
    CHECK(!s.save_account(other, false));                 // the plain path is closed
    CHECK(s.account_rows().size() == 1);
    keys.lock();
    CHECK(!keys.save(other, false));                      // locked: a new key cannot be sealed
    me.posts = 7;
    CHECK(keys.save(me, true));                           // but what is not secret can change
    CHECK(s.account_rows()[0].posts == 7 && s.account_rows()[0].secret.size() == 105);
    Account forged = me; forged.key = Keypair::generate();
    CHECK(!keys.save(forged, true));                      // not by naming another key
    CHECK(keys.unlock("pass") == Unlock::Ok && keys.save(other, false));
    for (const auto& r : s.account_rows()) CHECK(r.secret.size() == 105);
}

TEST(a_sealed_key_opens_only_for_its_own_account) {
    Store s(":memory:"); Keystore keys(s);
    Account a{"a", Keypair::generate(), 1}, b{"b", Keypair::generate(), 2};
    keys.save(a, true); keys.save(b, false);
    CHECK(keys.protect("pass", KdfParams::fastest()));
    auto rows = s.account_rows();
    // Swap the two sealed blobs: each now sits next to the other's public key.
    std::swap(rows[0].secret, rows[1].secret);
    s.put_account_row(rows[0]); s.put_account_row(rows[1]);
    for (const auto& x : keys.accounts()) CHECK(zero(x.key.sk));
    // A flipped byte in a blob is caught the same way.
    std::swap(rows[0].secret, rows[1].secret);
    rows[0].secret[40] ^= 1;
    s.put_account_row(rows[0]); s.put_account_row(rows[1]);
    const auto now = keys.accounts();
    CHECK(zero(now[0].key.sk) && now[1].key.sk == b.key.sk);
    CHECK(!keys.protect("other", KdfParams::fastest()));   // a key that does not open is never re-sealed away
}

TEST(the_passphrase_can_be_changed_and_removed_without_losing_a_key) {
    TempDir dir;
    Account me{"me", Keypair::generate(), 1};
    Store s(dir.db()); Keystore keys(s);
    keys.save(me, true);
    CHECK(keys.protect("one", KdfParams::fastest()));
    CHECK(keys.protect("two", KdfParams::fastest()));      // unlocked: changed
    keys.lock();
    CHECK(keys.unlock("one") == Unlock::WrongPassphrase && keys.unlock("two") == Unlock::Ok);
    CHECK(keys.accounts()[0].key.sk == me.key.sk);
    CHECK(keys.unprotect("two"));
    CHECK(!keys.is_protected() && !keys.is_unlocked() && !s.meta("check"));
    CHECK(keys.accounts()[0].key.sk == me.key.sk && s.save_account(me, true));   // plain again, as asked
}

TEST(unlocking_costs_argon2id_at_desktop_strength_and_a_damaged_setting_is_refused) {
    const auto d = KdfParams::desktop();
    CHECK(d.ops >= crypto_pwhash_OPSLIMIT_MODERATE && d.mem >= crypto_pwhash_MEMLIMIT_MODERATE);
    CHECK(d.ops >= crypto_pwhash_OPSLIMIT_INTERACTIVE && d.mem >= 256ull * 1024 * 1024);
    Store s(":memory:"); Keystore keys(s);
    keys.save(Account{"me", Keypair::generate(), 1}, true);
    CHECK(!keys.protect("x", KdfParams{0, crypto_pwhash_MEMLIMIT_MIN}));           // below libsodium's minimum
    CHECK(!keys.protect("x", KdfParams{1, 64ull << 30}));                           // 64 GiB
    CHECK(keys.protect("x", KdfParams::fastest()));
    const auto kdf = *s.meta("kdf");
    CHECK(kdf.size() == 34 && kdf[1] == crypto_pwhash_ALG_ARGON2ID13);
    // A store asking for no work at all, or for all the memory there is:
    // not unlocked with it, whatever the passphrase.
    for (int field : {2, 10}) {
        auto bad = kdf;
        for (int i = 0; i < 8; ++i) bad[field + i] = field == 2 ? 0 : 0xFF;
        s.set_meta("kdf", bad);
        keys.lock();
        CHECK(keys.unlock("x") == Unlock::Damaged && !keys.is_unlocked());
    }
    s.set_meta("kdf", kdf);
    CHECK(keys.unlock("x") == Unlock::Ok);
}

TEST(secret_keys_are_wiped_when_they_go_away) {
    alignas(Keypair) unsigned char buf[sizeof(Keypair)];
    auto* k = new (buf) Keypair(Keypair::generate());
    CHECK(!zero(k->sk));
    k->~Keypair();
    CHECK(sodium_is_zero(buf + offsetof(Keypair, sk), sizeof(SecretKey)) == 1);
    // Locking forgets the derived key: nothing opens until the next unlock.
    Store s(":memory:"); Keystore keys(s);
    keys.save(Account{"me", Keypair::generate(), 1}, true);
    keys.protect("p", KdfParams::fastest());
    CHECK(!zero(keys.accounts()[0].key.sk));
    keys.lock();
    CHECK(!keys.is_unlocked() && zero(keys.accounts()[0].key.sk));
}

// ─── Backup and restore ───────────────────────────────────────────────────────

TEST(recovery_words_are_the_bip39_english_list) {
    std::string list;
    for (size_t i = 0; i < 2048; ++i) list += std::string(recovery_word(i)) + "\n";
    uint8_t h[32];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(list.data()), list.size());
    CHECK(to_hex(h, 32) == "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda");
    // BIP-39's own 256-bit vectors: the same words for the same 32 bytes.
    auto words_for = [](uint8_t fill) {
        uint8_t seed[32]; std::memset(seed, fill, 32);
        Keypair k; crypto_sign_seed_keypair(k.pk.data(), k.sk.data(), seed);
        return recovery_phrase(k.sk);
    };
    std::string abandon, zoo;
    for (int i = 0; i < 23; ++i) { abandon += "abandon "; zoo += "zoo "; }
    CHECK(words_for(0x00) == abandon + "art");
    CHECK(words_for(0xFF) == zoo + "vote");
    CHECK(words_for(0x7F) == "legal winner thank year wave sausage worth useful legal winner thank year wave sausage "
                             "worth useful legal winner thank year wave sausage worth title");
    CHECK(words_for(0x80) == "letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount "
                             "doctor acoustic avoid letter advice cage absurd amount doctor acoustic bless");
}

TEST(a_restored_account_has_the_same_public_key_on_a_fresh_install) {
    Account original{"me", Keypair::generate(), 1};
    std::string words = recovery_phrase(original.key.sk);
    CHECK(std::count(words.begin(), words.end(), ' ') == 23);
    TempDir fresh;
    Store s(fresh.db()); Keystore keys(s);
    std::string messy = "  " + words + "\n";
    for (auto& c : messy) if (c == ' ') { c = '\t'; break; }
    for (size_t i = 0; i < messy.size(); i += 7) messy[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(messy[i])));
    for (const auto& phrase : {words, messy}) {
        std::string why;
        const auto back = restore_from_phrase(phrase, &why);
        CHECK(back && back->pk == original.key.pk && back->sk == original.key.sk && why.empty());
    }
    const auto k = restore_from_phrase(words);
    Account restored{"restored", *k, 2, 0, true};
    CHECK(keys.save(restored, true));
    const Post p = author_as(restored, topic("back", "after a restore"));
    CHECK(verify(p) == Check::Ok && p.author == original.key.pk);
    sodium_memzero(words.data(), words.size());
}

TEST(a_mistyped_missing_or_swapped_word_is_caught) {
    uint8_t seed[32];
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 3);
    Keypair k; crypto_sign_seed_keypair(k.pk.data(), k.sk.data(), seed);
    const std::string words = recovery_phrase(k.sk);
    std::vector<std::string> w;
    for (size_t at = 0, sp; at <= words.size(); at = sp + 1) {
        sp = words.find(' ', at); if (sp == std::string::npos) sp = words.size();
        w.push_back(words.substr(at, sp - at));
    }
    CHECK(w.size() == 24);
    auto join = [](const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += x + " "; return s; };
    std::string why;
    // A word not in the list is named by its position.
    auto typo = w; typo[6] += "x";
    CHECK(!restore_from_phrase(join(typo), &why) && why.find("word 7") != std::string::npos);
    // 23 or 25 words.
    auto shorter = w; shorter.pop_back();
    CHECK(!restore_from_phrase(join(shorter), &why) && why.find("24 words") != std::string::npos);
    auto longer = w; longer.push_back("abandon");
    CHECK(!restore_from_phrase(join(longer)));
    // A real word in the wrong place is a matter of the 8-bit checksum: it
    // catches 255 in 256. Measured over 200 phrases: each word replaced by
    // its neighbour in the list, and each pair of neighbours swapped.
    int missed = 0, tried = 0;
    for (int n = 0; n < 200; ++n) {
        uint8_t sd[32];
        for (int i = 0; i < 32; ++i) sd[i] = static_cast<uint8_t>(n * 31 + i * 7 + 3);
        Keypair kn; crypto_sign_seed_keypair(kn.pk.data(), kn.sk.data(), sd);
        const std::string phrase = recovery_phrase(kn.sk);
        std::vector<std::string> v;
        for (size_t at = 0, sp; at <= phrase.size(); at = sp + 1) {
            sp = phrase.find(' ', at); if (sp == std::string::npos) sp = phrase.size();
            v.push_back(phrase.substr(at, sp - at));
        }
        for (size_t i = 0; i < 24; ++i) {
            auto sub = v;
            for (size_t j = 0; j < 2048; ++j)
                if (sub[i] == recovery_word(j)) { sub[i] = recovery_word((j + 1) % 2048); break; }
            ++tried; missed += restore_from_phrase(join(sub)).has_value();
            if (i + 1 < 24 && v[i] != v[i + 1]) {
                auto sw = v; std::swap(sw[i], sw[i + 1]);
                ++tried; missed += restore_from_phrase(join(sw)).has_value();
            }
        }
    }
    CHECK(tried > 9000 && missed * 128 < tried);   // under 1 in 128 (expected: 1 in 256)
    CHECK(!restore_from_phrase(join(w) + "\xF0\x9F\x94\x91"));   // trailing junk is a 25th word
}

TEST(restore_never_reuses_anonymous_keys_and_they_are_never_derived_from_the_account) {
    // The original install: an account, and two anonymous posts.
    Bus bus; FakeNet na(bus); Store sa(":memory:");
    Engine ea(sa, na, "Logos Forum"); na.engine = &ea;
    Account me{"me", Keypair::generate(), 1};
    std::set<PublicKey> keys{me.key.pk};
    size_t anonymous = 0;
    const std::string t = ea.post_topic(&me, "mine", "signed by the account", "", 1000);
    for (int i = 0; i < 2; ++i) {
        const auto p = sa.get(ea.post_reply(nullptr, t, "anonymous " + std::to_string(i), "", 2000 + i));
        keys.insert(p->author); ++anonymous;
    }
    const std::string words = recovery_phrase(me.key.sk);
    // Two fresh installs restore the same words, then each posts anonymously
    // twice, with the very same text and dates as the other. Were anonymous
    // keys derived from the account, both installs would sign with the same
    // keys; were they restored, they would repeat the original's.
    for (int install = 0; install < 2; ++install) {
        Store s(":memory:"); Keystore ks(s);
        Account back{"me", *restore_from_phrase(words), 5000, 0, true};
        CHECK(back.key.pk == me.key.pk);
        ks.save(back, true);
        FakeNet n(bus); Engine e(s, n, "Logos Forum"); n.engine = &e;
        for (int i = 0; i < 2; ++i) {
            const auto p = s.get(e.post_reply(nullptr, t, "after a restore " + std::to_string(i), "", 9000 + i));
            CHECK(p && p->mode == Mode::Anonymous && verify(*p) == Check::Ok);
            keys.insert(p->author); ++anonymous;
        }
        CHECK(s.account_rows().size() == 1);   // nothing about anonymous posts came back with the words
    }
    // Six anonymous keys and the account's: seven different keys.
    CHECK(anonymous == 6 && keys.size() == 7);
}

TEST(a_rotation_marks_the_account_as_not_backed_up_and_the_old_words_give_the_old_key) {
    TempDir dir;
    Store s(dir.db()); Keystore keys(s);
    Account me{"me", Keypair::generate(), 1};
    const std::string words = recovery_phrase(me.key.sk);
    me.backed_up = true;
    keys.save(me, true);
    CHECK(keys.accounts()[0].backed_up);
    const PublicKey old = rotate(me, 2);
    CHECK(!me.backed_up);
    keys.save(me, true);
    CHECK(!keys.accounts()[0].backed_up);
    // The words written before restore the retired key, never the new one:
    // the account does not silently come back as what it rotated away from.
    const auto back = restore_from_phrase(words);
    CHECK(back && back->pk == old && back->pk != me.key.pk);
    CHECK(restore_from_phrase(recovery_phrase(me.key.sk))->pk == me.key.pk);
}

