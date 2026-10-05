// SPDX-License-Identifier: MIT OR Apache-2.0
// The forum's core rules, each checked by a test named for what it guarantees.
#include "forum/dns.h"
#include "forum/engine.h"
#include "forum/identity.h"
#include "forum/store.h"
#include "forum/post.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace forum;

static int failures = 0, ran = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
struct T { const char* name; std::function<void()> f; };
static std::vector<T>& all() { static std::vector<T> v; return v; }
#define TEST(n) static void n(); static const bool reg_##n = (all().push_back({#n, n}), true); static void n()

static Post topic(const std::string& title = "Hello", const std::string& body = "First post") {
    Post p; p.kind = Kind::Topic; p.forum = "Logos Forum"; p.title = title; p.body = body; p.ts_ms = 1790000000000; return p;
}

TEST(a_signed_post_round_trips_and_verifies) {
    Account a{"me", Keypair::generate()};
    const Post p = author_as(a, topic());
    Check why;
    const auto back = decode(encode(p), &why);
    CHECK(back.has_value());
    CHECK(why == Check::Ok);
    CHECK(back && back->id() == p.id());
    CHECK(back && back->author == a.key.pk);
}

TEST(a_tampered_post_is_refused) {
    Account a{"me", Keypair::generate()};
    Post p = author_as(a, topic());
    p.body = "edited after signing";
    Check why;
    CHECK(!decode(encode(p), &why).has_value());
    CHECK(why == Check::BadSignature);
}

TEST(nobody_can_post_under_another_key) {
    Account victim{"victim", Keypair::generate()}, forger{"forger", Keypair::generate()};
    Post p = topic();
    p.author = victim.key.pk;          // claims to be the victim
    sign(p, forger.key.sk);            // but only holds their own key
    CHECK(verify(p) == Check::BadSignature);
}

TEST(the_id_is_the_content_so_any_change_moves_it) {
    Account a{"me", Keypair::generate()};
    const Post p = author_as(a, topic("A"));
    const Post q = author_as(a, topic("B"));
    CHECK(p.id().size() == 64);
    CHECK(p.id() != q.id());
}

TEST(two_anonymous_posts_share_no_key) {
    const Post p = author_anonymously(topic("one"));
    const Post q = author_anonymously(topic("two"));
    CHECK(verify(p) == Check::Ok);
    CHECK(verify(q) == Check::Ok);
    CHECK(p.author != q.author);
    CHECK(p.mode == Mode::Anonymous && p.alias.empty());
}

TEST(an_alias_is_signed_by_the_account_and_shown_by_name) {
    Account a{"me", Keypair::generate()};
    const Post p = author_as(a, topic(), "night-owl");
    CHECK(p.mode == Mode::Alias && p.alias == "night-owl");
    const auto back = decode(encode(p));
    CHECK(back && back->alias == "night-owl" && back->author == a.key.pk);
}

TEST(a_reply_needs_a_topic_and_a_topic_needs_a_title) {
    Account a{"me", Keypair::generate()};
    Post r; r.kind = Kind::Reply; r.forum = "Logos Forum"; r.body = "hi";
    CHECK(verify(author_as(a, r)) == Check::MissingTopic);
    r.topic_id = author_as(a, topic()).id();
    CHECK(verify(author_as(a, r)) == Check::Ok);
    CHECK(verify(author_as(a, topic("", "no title"))) == Check::MissingTitle);
}

TEST(oversized_posts_are_refused) {
    Account a{"me", Keypair::generate()};
    CHECK(verify(author_as(a, topic("t", std::string(kMaxBody + 1, 'x')))) == Check::TooLong);
}

TEST(non_forum_traffic_is_ignored) {
    CHECK(!decode("not json").has_value());
    CHECK(!decode("{\"hello\":1}").has_value());
    CHECK(!decode("[]").has_value());
}

TEST(rotation_retires_the_key_after_the_policy_limit) {
    Account a{"me", Keypair::generate(), 1000};
    RotationPolicy pol{2, 0};
    author_as(a, topic("1"));
    CHECK(!pol.due(a, 2000));
    author_as(a, topic("2"));
    CHECK(pol.due(a, 2000));
    const PublicKey old = rotate(a, 3000);
    CHECK(old != a.key.pk && a.posts == 0 && a.label == "me");
    const RotationPolicy by_age{0, 500};
    CHECK(by_age.due(a, 3600) && !by_age.due(a, 3400));
}


// ------------------------------------------------ a fake network that can be cut

struct Bus {
    std::vector<std::string> log;           // everything ever published (the "store node")
    std::vector<struct FakeNet*> peers;
};
struct FakeNet : Transport {
    Bus& bus; bool up = true; Engine* engine = nullptr; uint64_t now = 0;
    explicit FakeNet(Bus& b) : bus(b) { bus.peers.push_back(this); }
    SendResult send(const std::string&, const std::string& payload) override {
        if (!up) return {false, "offline"};
        bus.log.push_back(payload);
        for (auto* p : bus.peers) if (p != this && p->up && p->engine) p->engine->receive(payload, now);
        return {true, "", "", true};
    }
    bool subscribe(const std::string&) override { return true; }
    std::vector<std::string> history(const std::string&) override { return up ? bus.log : std::vector<std::string>{}; }
};

TEST(a_post_written_offline_is_kept_and_sent_when_back_online) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    Account me{"me", Keypair::generate()};
    a.up = false;
    const std::string id = ea.post_topic(&me, "offline", "written on a train", "", 1000);
    CHECK(sa.has(id));                          // shows up locally at once
    CHECK(ea.pump(1000) == 0);                  // the send fails…
    CHECK(sa.outbox().size() == 1 && sa.outbox()[0].attempts == 1);
    CHECK(ea.pump(2500) == 0 && sa.outbox()[0].attempts == 1);   // …and waits out its back-off
    a.up = true;
    CHECK(ea.pump(3000) == 1);                  // retried once due
    CHECK(sa.outbox().empty() && sb.has(id));
}

TEST(peers_see_each_other_and_duplicates_are_stored_once) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    Account alice{"alice", Keypair::generate()}, bob{"bob", Keypair::generate()};
    const std::string t = ea.post_topic(&alice, "Privacy", "What do you use?", "", 10);
    ea.pump(10);
    eb.post_reply(&bob, t, "Logos, obviously", "", 20);
    eb.pump(20);
    CHECK(sa.replies(t).size() == 1 && sb.replies(t).size() == 1);
    CHECK(!eb.receive(bus.log[0], 30));        // the same post again changes nothing
    CHECK(sb.count() == 2);
}

TEST(coming_back_online_catches_up_on_what_was_missed) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    Account alice{"alice", Keypair::generate()};
    b.up = false;                               // bob is away
    for (int i = 0; i < 3; ++i) { ea.post_topic(&alice, "t" + std::to_string(i), "b", "", 100 + i); }
    ea.pump(200);
    CHECK(sb.count() == 0);
    b.up = true;
    CHECK(eb.catch_up(300) == 3 && sb.count() == 3);
    CHECK(eb.catch_up(301) == 0);               // history twice adds nothing
}

TEST(a_burst_is_paced_not_dropped) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    Account me{"me", Keypair::generate()};
    for (int i = 0; i < 12; ++i) ea.post_topic(&me, "t" + std::to_string(i), "b", "", 1000);
    CHECK(ea.pump(1000) == 5);                  // the bucket holds five
    CHECK(sa.outbox().size() == 7);             // the rest wait, none is lost
    CHECK(ea.pump(1000 + 60000) == 5);          // a minute refills the bucket to five…
    CHECK(ea.pump(1000 + 120000) == 2);         // …and the last two follow
    CHECK(sa.outbox().empty());
}

TEST(forged_and_foreign_traffic_is_ignored) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    Account x{"x", Keypair::generate()};
    Post p = author_as(x, topic()); p.body = "tampered";
    CHECK(!ea.receive(encode(p), 1));
    Post other = author_as(x, topic()); other.forum = "Another Forum"; sign(other, x.key.sk);
    CHECK(!ea.receive(encode(other), 1));
    CHECK(sa.count() == 0);
}

TEST(topics_list_by_latest_activity_with_reply_counts) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    Account me{"me", Keypair::generate()};
    const std::string old_t = ea.post_topic(&me, "older", "b", "", 100);
    const std::string new_t = ea.post_topic(&me, "newer", "b", "", 200);
    ea.post_reply(nullptr, old_t, "anonymous bump", "", 300);
    const auto ts = sa.topics("Logos Forum");
    CHECK(ts.size() == 2 && ts[0].id == old_t && ts[0].replies == 1 && ts[1].id == new_t);
    CHECK(sa.replies(old_t)[0].mode == Mode::Anonymous);
}

TEST(the_store_and_accounts_survive_a_restart) {
    const std::string path = "/tmp/forum-core-test.db";
    std::remove(path.c_str());
    std::string id;
    {
        Store s(path); Bus bus; FakeNet a(bus); a.up = false; Engine e(s, a, "Logos Forum");
        Account me{"me", Keypair::generate(), 5};
        s.save_account(me, true);
        id = e.post_topic(&me, "kept", "across restarts", "", 10);
    }
    Store s(path);
    CHECK(s.has(id) && s.outbox().size() == 1);
    CHECK(s.accounts().size() == 1 && s.selected_label() == std::string("me"));
    std::remove(path.c_str());
}

// Logos Delivery's shape: it accepts every send, then delivers or not, later.
struct AcceptingNet : Transport {
    std::vector<std::string> sent;
    SendResult send(const std::string&, const std::string& payload) override {
        sent.push_back(payload);
        return {true, "", "req-" + std::to_string(sent.size()), false};
    }
    bool subscribe(const std::string&) override { return true; }
    std::vector<std::string> history(const std::string&) override { return {}; }
};

TEST(an_accepted_send_stays_in_the_outbox_until_the_network_confirms_it) {
    const std::string path = "/tmp/forum-core-confirm.db";
    std::remove(path.c_str());
    AcceptingNet net;
    Account me{"me", Keypair::generate()};
    std::string id;
    {
        Store s(path); Engine e(s, net, "Logos Forum");
        id = e.post_topic(&me, "offline", "accepted, never confirmed", "", 1000);
        CHECK(e.pump(1000) == 1 && net.sent.size() == 1);
        CHECK(s.outbox().size() == 1);                              // kept: acceptance is not delivery
        CHECK(e.pump(1000 + 1000) == 0 && net.sent.size() == 1);    // not resent while awaiting…
    }                                                               // …and the app is closed here
    Store s(path); Engine e(s, net, "Logos Forum");
    CHECK(s.outbox().size() == 1);                                  // the post survived the restart
    CHECK(e.pump(1000 + Engine::kConfirmWindowMs) == 1 && net.sent.size() == 2);  // sent again
    CHECK(e.reconnected(1000 + Engine::kConfirmWindowMs + 5) == 1 && net.sent.size() == 3);  // back online: at once
    e.confirm(id);
    CHECK(s.outbox().empty());
    std::remove(path.c_str());
}

TEST(a_send_lost_after_acceptance_goes_back_in_the_outbox) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    Account me{"me", Keypair::generate()};
    std::string sent_id;
    ea.on_sent = [&](const std::string& id, const std::string&) { sent_id = id; };
    const std::string id = ea.post_topic(&me, "t", "b", "", 10);
    CHECK(ea.pump(10) == 1 && sent_id == id && sa.outbox().empty());
    CHECK(ea.requeue(id, "messageError: no peers", 20));   // the network lost it later
    CHECK(sa.outbox().size() == 1 && sa.outbox()[0].attempts == 1);
    CHECK(ea.pump(20 + backoff_ms(1)) == 1 && sa.outbox().empty());
    CHECK(!ea.requeue("not-a-post-of-ours", "x", 30));
}

TEST(a_snapshot_restores_a_forum_on_a_fresh_install) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum");
    Account me{"me", Keypair::generate()};
    const std::string t = ea.post_topic(&me, "kept", "in storage", "", 10);
    ea.post_reply(nullptr, t, "anon", "", 20);
    ea.post_reply(&me, t, "as an alias", "Ghost", 30);
    const std::string doc = ea.snapshot();
    CHECK(eb.import_snapshot(doc, 40) == 3 && sb.replies(t).size() == 2);
    CHECK(eb.import_snapshot(doc, 41) == 0);                  // idempotent
    Engine other(sb, b, "Another Forum");
    CHECK(other.import_snapshot(doc, 42) == 0);               // not this forum's
}

TEST(a_snapshot_is_checked_post_by_post) {
    Bus bus; FakeNet a(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, a, "Logos Forum");
    Account me{"me", Keypair::generate()};
    ea.post_topic(&me, "genuine", "b", "", 10);
    Post forged = author_as(me, topic("forged")); forged.body = "edited";
    auto doc = nlohmann::json::parse(ea.snapshot());
    doc["posts"].push_back(encode(forged));
    doc["posts"].push_back("not even json");
    CHECK(eb.import_snapshot(doc.dump(), 20) == 1 && sb.count() == 1);
    CHECK(eb.import_snapshot("garbage", 20) == 0);
}

TEST(a_snapshot_announcement_reaches_peers_and_is_not_a_post) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    std::string got; size_t n = 0;
    eb.on_snapshot = [&](const Announcement& an) { got = an.cid; n = an.posts; };
    const std::string re = eb.request_history(9);
    ea.announce_snapshot("zDvZRwzkwWtSfgKFoPMRaxzAAJ1i", 7, 10, re);
    CHECK(got == "zDvZRwzkwWtSfgKFoPMRaxzAAJ1i" && n == 7 && sb.count() == 0);
    got.clear();
    Engine elsewhere(sb, b, "Another Forum");
    elsewhere.on_snapshot = [&](const Announcement& an) { got = an.cid; };
    CHECK(!elsewhere.receive(bus.log.back(), 11) && got.empty());   // another forum's pointer is ignored
}

TEST(a_node_back_from_offline_gets_history_from_a_peer_through_storage) {
    // No store node keeps anything (the Bus log is cleared, as on logos.test),
    // so history has to come from a peer: B asks, A answers with a snapshot,
    // "storage" is a map from CID to bytes, B fetches and merges.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    std::map<std::string, std::string> storage;
    int answers = 0;
    ea.jitter = [] { return uint64_t{0}; };
    ea.on_history_wanted = [&](const HistoryRequest& r) {
        ++answers;
        const std::string doc = ea.snapshot();
        const std::string cid = "cid-" + std::to_string(std::hash<std::string>{}(doc));
        storage[cid] = doc;
        ea.announce_snapshot(cid, sa.count(), 0, r.id);
    };
    eb.on_snapshot = [&](const Announcement& an) {
        CHECK(an.peer == "16Uiu2-alice-storage" && an.addrs.size() == 1);   // B can dial A directly
        eb.import_snapshot(storage.at(an.cid), 50);
    };
    ea.set_storage_provider("16Uiu2-alice-storage", {"/ip4/127.0.0.1/tcp/20001"});
    Account alice{"alice", Keypair::generate()};
    b.up = false;
    const std::string t = ea.post_topic(&alice, "while you were out", "b", "", 10);
    ea.post_reply(nullptr, t, "anon", "", 11);
    ea.pump(12);
    bus.log.clear();                            // the network kept nothing
    b.up = true;
    CHECK(eb.catch_up(40) == 0);                // store query: nothing
    eb.request_history(41);
    CHECK(answers == 0);                        // nobody answers at once…
    ea.tick(41);
    CHECK(answers == 1 && sb.count() == 2 && sb.replies(t).size() == 1);   // …but after the wait
    eb.request_history(42);                     // asked again at once: A stays quiet
    ea.tick(42 + Engine::kAnswerJitterMs);
    CHECK(answers == 1);
    ea.request_history(43);                     // B holds as many as A claims: B stays quiet
    eb.tick(43 + Engine::kAnswerJitterMs);
    CHECK(answers == 1);
}

TEST(one_newcomer_costs_one_answer_not_one_per_peer) {
    // Five peers hold the forum; a newcomer asks. Each peer waits a different
    // time; the first to answer is seen by the rest, who stand down.
    Bus bus; std::vector<std::unique_ptr<FakeNet>> nets; std::vector<std::unique_ptr<Store>> stores;
    std::vector<std::unique_ptr<Engine>> peers;
    Account alice{"alice", Keypair::generate()};
    Post shared = author_as(alice, topic("shared history"));
    int answers = 0;
    for (int i = 0; i < 5; ++i) {
        nets.push_back(std::make_unique<FakeNet>(bus));
        stores.push_back(std::make_unique<Store>(":memory:"));
        peers.push_back(std::make_unique<Engine>(*stores.back(), *nets.back(), "Logos Forum"));
        nets.back()->engine = peers.back().get();
        stores.back()->put(shared, 1);
        Engine* e = peers.back().get();
        e->jitter = [i] { return uint64_t(500 * (i + 1)); };
        e->on_history_wanted = [&answers, e](const HistoryRequest& r) { ++answers; e->announce_snapshot("cid-x", 1, 0, r.id); };
    }
    FakeNet nn(bus); Store sn(":memory:"); Engine newcomer(sn, nn, "Logos Forum"); nn.engine = &newcomer;
    newcomer.request_history(1000);
    for (uint64_t t = 1000; t <= 1000 + Engine::kAnswerJitterMs; t += 100)
        for (auto& p : peers) p->tick(t);
    CHECK(answers == 1);
}

TEST(history_crosses_nat_over_delivery_when_storage_cannot) {
    // B could not fetch A's snapshot (their storage nodes cannot reach each
    // other), so it asks again via Delivery: A sends the posts themselves, in
    // bundles under the network's message size, and B checks every one.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    b.up = false;
    const std::string big(900, 'x');
    for (int i = 0; i < 300; ++i) ea.post_topic(&alice, "t" + std::to_string(i), big, "", 1000 + i);
    for (uint64_t t = 1000; sa.outbox().size(); t += 60000) ea.pump(t);
    bus.log.clear();
    b.up = true;
    const size_t before = bus.log.size();
    eb.request_history(5000000, /*via_delivery=*/true);
    ea.tick(5000000);
    for (uint64_t t = 5000000; t < 5000000 + 60000; t += 3000) ea.pump(t);   // answers are paced like any send
    size_t bundles = 0, largest = 0, total = 0;
    for (size_t i = before; i < bus.log.size(); ++i)
        if (bus.log[i].find("logos-forum-snapshot\"") != std::string::npos) {
            ++bundles; largest = std::max(largest, bus.log[i].size()); total += bus.log[i].size();
        }
    CHECK(bundles >= 2 && largest < 150 * 1024);               // split under the network's limit
    CHECK(total <= Engine::kMaxAnswerBytes);                    // and capped, whatever was asked
    CHECK(sb.count() > 150 && sb.count() < 300);                // the most recent part of the history…
    CHECK(sb.topics("Logos Forum").front().topic.title == "t299");   // …newest first
    // A forged post inside a bundle is dropped like any other.
    Post forged = author_as(alice, topic("forged")); forged.body = "edited";
    auto doc = nlohmann::json::parse(ea.bundles(0, "r")[0]);
    doc["posts"].push_back(encode(forged));
    const size_t had = sb.count();
    CHECK(!eb.receive(doc.dump(), 6000000) && sb.count() == had);
}

TEST(the_delivery_fallback_is_answered_right_after_a_storage_answer) {
    // What happened between a Mac and a Linux node behind Docker's NAT: A
    // answers B's Storage request, B cannot fetch the snapshot and asks again
    // via Delivery three seconds later. That second answer must still come.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    int storage_answers = 0;
    ea.on_history_wanted = [&](const HistoryRequest& r) { ++storage_answers; ea.announce_snapshot("cid-unreachable", 1, 0, r.id); };
    Account alice{"alice", Keypair::generate()};
    b.up = false;
    ea.post_topic(&alice, "t", "b", "", 1000);
    ea.pump(1000);
    b.up = true;
    eb.request_history(2000);
    ea.tick(2000);
    CHECK(storage_answers == 1 && sb.count() == 0);             // announced, but B cannot fetch it
    eb.request_history(5000, true);
    ea.tick(5000); ea.pump(5000);
    CHECK(sb.count() == 1);                                     // the Delivery answer still came
    eb.request_history(6000, true);                             // and the per-path limit still holds
    ea.tick(6000); ea.pump(6000);
    CHECK(sb.count() == 1);
}

TEST(a_returning_node_asks_only_for_what_is_newer) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    const std::string old_t = ea.post_topic(&alice, "old", "b", "", 1000);
    ea.pump(1000);                                              // B has this one
    b.up = false;
    ea.post_topic(&alice, "new", "b", "", 10'000'000);
    ea.pump(10'000'000);
    b.up = true;
    eb.request_history(10'000'100, true);
    ea.tick(10'000'100); ea.pump(10'000'100);
    CHECK(sb.count() == 2 && sb.has(old_t));
    CHECK(ea.bundles(10'000'000 - 3600000, "r").size() == 1 &&
          nlohmann::json::parse(ea.bundles(10'000'000 - 3600000, "r")[0])["posts"].size() == 1);  // only the new one
}

TEST(mistyped_traffic_is_ignored_not_fatal) {
    // Anyone can publish on the topic; a field of the wrong type must be a
    // dropped message, never an exception that takes the app down.
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum");
    int calls = 0;
    ea.on_snapshot = [&](const Announcement&) { ++calls; };
    const std::string re = ea.request_history(1);
    const std::string r = R"(,"re":")" + re + "\"";
    for (const std::string m : {
             R"({"logos-forum-snapshot-at":"1","forum":"Logos Forum","cid":"x")" + r + "}",
             R"({"logos-forum-snapshot-at":1,"forum":5,"cid":"x")" + r + "}",
             R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":null)" + r + "}",
             R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":"x","posts":"many")" + r + "}",
             std::string(R"({"v":"1","kind":7})"), std::string("[]"), std::string("null"), std::string(""), std::string("{")}) {
        CHECK(!ea.receive(m, 1));
    }
    CHECK(calls == 1);  // only the last well-formed pointer, with a bad count read as 0
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":"1","forum":"Logos Forum","posts":[]})", 1) == 0);
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":1,"forum":null,"posts":[]})", 1) == 0);
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":1,"forum":"Logos Forum","posts":{"a":1}})", 1) == 0);
    CHECK(sa.count() == 0);
}

// ------------------------------------------------ what unsigned traffic may do (review #1)

TEST(an_unsolicited_announcement_makes_nobody_fetch_anything) {
    // Announcements are unsigned. Acted on only as the answer to a request of
    // ours, while it is open, and only once: nobody can make every reader dial
    // them by announcing a fresh CID.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    int fetches = 0;
    eb.on_snapshot = [&](const Announcement&) { ++fetches; };
    ea.announce_snapshot("cid-1", 5, 10);                        // nobody asked
    ea.announce_snapshot("cid-2", 5, 10, "made-up-request");     // not a request of B's
    CHECK(fetches == 0);
    const std::string re = eb.request_history(20);
    ea.announce_snapshot("cid-3", 5, 21, re);
    ea.announce_snapshot("cid-4", 5, 22, re);                    // a second answer to the same request
    CHECK(fetches == 1);
    const std::string late = eb.request_history(100);
    a.now = 100 + Engine::kRequestOpenMs + 1;                 // the fake network passes the sender's clock
    ea.announce_snapshot("cid-5", 5, 0, late);                   // after the request closed
    CHECK(fetches == 1);
}

TEST(a_snapshot_carries_posts_and_nothing_else) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    int fetches = 0, answers = 0;
    eb.jitter = [] { return uint64_t{0}; };
    eb.on_snapshot = [&](const Announcement&) { ++fetches; };
    eb.on_history_wanted = [&](const HistoryRequest&) { ++answers; };
    Account alice{"alice", Keypair::generate()};
    auto doc = nlohmann::json::parse(ea.snapshot());
    doc["posts"].push_back(encode(author_as(alice, topic("a real post"))));
    doc["posts"].push_back(R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":"evil"})");
    doc["posts"].push_back(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"id":"x"})");
    sb.put(author_as(alice, topic("so B has one")), 1);
    CHECK(eb.import_snapshot(doc.dump(), 50) == 1);
    eb.tick(50 + Engine::kAnswerJitterMs);
    CHECK(fetches == 0 && answers == 0);
}

TEST(a_history_request_cannot_make_a_node_flood_the_topic) {
    // Ten tiny requests with fresh ids, one every 30 s: what a node puts on the
    // network is bounded by the answer cap, the Delivery answer interval and
    // the rate limit, not by the size of its history.
    Bus bus; FakeNet a(bus), x(bus); Store sa(":memory:"), sx(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    const std::string big(2000, 'x');
    for (int i = 0; i < 300; ++i) sa.put(author_as(alice, topic("t" + std::to_string(i), big)), 1);
    const size_t before = bus.log.size();
    uint64_t t = 1000;
    for (int i = 0; i < 10; ++i, t += 30000) {
        a.now = t;
        ea.receive(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"via":"delivery","since":0,"id":"evil)" +
                       std::to_string(i) + "\"}", t);
        for (uint64_t u = t; u < t + 30000; u += 3000) { ea.tick(u); ea.pump(u); }
    }
    size_t bytes = 0;
    for (size_t i = before; i < bus.log.size(); ++i) bytes += bus.log[i].size();
    CHECK(bytes <= 5 * Engine::kMaxAnswerBytes);                // at most one answer a minute over five minutes
    CHECK(bytes < 1500 * 1024);                                 // was 6.5 MB
}

TEST(sustained_requests_cost_a_node_a_bounded_budget) {
    // One tiny request a minute for three hours: what a node sends stays within
    // its hourly budget, not proportional to the requests.
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    const std::string big(2000, 'x');
    for (int i = 0; i < 300; ++i) sa.put(author_as(alice, topic("t" + std::to_string(i), big)), 1);
    const size_t before = bus.log.size();
    for (int m = 0; m < 180; ++m) {
        const uint64_t t = 1000 + uint64_t(m) * 60000;
        a.now = t;
        ea.receive(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"via":"delivery","since":0,"id":"s)" +
                       std::to_string(m) + "\"}", t);
        for (uint64_t u = t; u < t + 60000; u += 6000) { ea.tick(u); ea.pump(u); }
    }
    size_t bytes = 0;
    for (size_t i = before; i < bus.log.size(); ++i) bytes += bus.log[i].size();
    CHECK(bytes <= 3 * Engine::kAnswerBudgetBytes);             // three hours, three budgets
}

TEST(an_empty_or_forged_bundle_does_not_silence_the_answerers) {
    Bus bus; FakeNet a(bus), x(bus); Store sa(":memory:"), sx(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    ea.jitter = [] { return uint64_t{1000}; };
    Account alice{"alice", Keypair::generate()};
    sa.put(author_as(alice, topic("history")), 1);
    const size_t before = bus.log.size();
    ea.receive(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"via":"delivery","since":0,"id":"r1"})", 10);
    Post forged = author_as(alice, topic("x")); forged.body = "edited";
    const nlohmann::json fake{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", "r1"},
                              {"posts", nlohmann::json::array({encode(forged)})}};
    ea.receive(fake.dump(), 20);                                 // "already answered", with nothing real in it
    ea.tick(2000); ea.pump(2000);
    bool answered = false;
    for (size_t i = before; i < bus.log.size(); ++i) answered = answered || bus.log[i].find("\"re\":\"r1\"") != std::string::npos;
    CHECK(answered);
}

TEST(a_replayed_partial_bundle_does_not_silence_the_answerers) {
    // Every post is public, so anyone can replay one. Standing down needs the
    // bundle to carry what we would have sent: our newest posts in the range.
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    ea.jitter = [] { return uint64_t{1000}; };
    Account alice{"alice", Keypair::generate()};
    Post old = topic("old"); old.ts_ms = 1000; const Post o = author_as(alice, old);
    Post fresh = topic("fresh"); fresh.ts_ms = 5000;
    sa.put(o, 1); sa.put(author_as(alice, fresh), 1);
    const size_t before = bus.log.size();
    ea.receive(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"via":"delivery","since":0,"id":"r1"})", 10);
    const nlohmann::json replay{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", "r1"},
                                {"posts", nlohmann::json::array({encode(o)})}};
    ea.receive(replay.dump(), 20);
    ea.receive(R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":"x","re":"r1"})", 30);  // nor does an announcement
    ea.tick(2000); ea.pump(2000);
    bool answered = false;
    for (size_t i = before; i < bus.log.size(); ++i) answered = answered || bus.log[i].find("\"re\":\"r1\"") != std::string::npos;
    CHECK(answered);
}

TEST(a_node_away_for_long_gets_everything_it_missed_over_delivery) {
    // Answers are capped; a full one says so, and the asker pages down until
    // it has everything since it left.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    const std::string big(2000, 'x');
    for (int i = 0; i < 300; ++i) { Post p = topic("t" + std::to_string(i), big); p.ts_ms = 1'000'000 + i * 1000; sa.put(author_as(alice, p), 1); }
    uint64_t t = 10'000'000;
    a.now = b.now = t;
    eb.request_history(t, true);
    for (int step = 0; step < 400 && sb.count() < 300; ++step, t += 6000) {
        a.now = b.now = t;
        ea.tick(t); ea.pump(t); eb.tick(t);
    }
    CHECK(sb.count() == 300);
}

// Drives two nodes like the backend does: A answers, B asks every five minutes
// (its periodic catch-up) and follows pages; returns how many posts B holds.
static size_t catch_up_over_hours(Engine& ea, Engine& eb, FakeNet& a, FakeNet& b, Store& sb, uint64_t t0, double hours,
                                  const std::function<void(uint64_t)>& attacker = {}) {
    uint64_t t = t0;
    for (uint64_t end = t0 + uint64_t(hours * 3600000); t < end; t += 5000) {
        a.now = b.now = t;
        if ((t - t0) % 300000 == 0) eb.request_history(t, true);
        if (attacker) attacker(t);
        ea.tick(t); ea.pump(t); eb.tick(t);
    }
    return sb.count();
}

TEST(a_backlog_larger_than_the_hourly_budget_still_arrives) {
    // 1,500 posts of 2 KB behind one peer: more than its 1 MB an hour. The
    // pages carry on across the periodic requests from where they stopped.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    const std::string big(2000, 'x');
    for (int i = 0; i < 1500; ++i) { Post p = topic("t" + std::to_string(i), big); p.ts_ms = 1'000'000 + i * 1000; sa.put(author_as(alice, p), 1); }
    CHECK(catch_up_over_hours(ea, eb, a, b, sb, 10'000'000, 6) == 1500);
}

TEST(posts_sharing_one_date_do_not_loop_the_pages) {
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    const std::string big(2000, 'x');
    for (int i = 0; i < 300; ++i) { Post p = topic("t" + std::to_string(i), big); p.ts_ms = 2'000'000; sa.put(author_anonymously(p), 1); }
    CHECK(catch_up_over_hours(ea, eb, a, b, sb, 10'000'000, 0.25) == 300);   // three pages, not a crawl
}

TEST(a_forged_page_marker_cannot_derail_the_paging) {
    // An attacker answers every request with "more" and no posts, or with a
    // replayed old post and "more": neither moves B's cursor past what it has
    // not received from A.
    Bus bus; FakeNet a(bus), b(bus), x(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{2000}; };
    Account alice{"alice", Keypair::generate()};
    const std::string big(2000, 'x');
    std::string oldest_post;
    for (int i = 0; i < 300; ++i) {
        Post p = topic("t" + std::to_string(i), big); p.ts_ms = 1'000'000 + i * 1000;
        const Post signed_post = author_as(alice, p); sa.put(signed_post, 1);
        if (i == 0) oldest_post = encode(signed_post);
    }
    size_t seen = 0;
    auto attacker = [&](uint64_t t) {
        for (; seen < bus.log.size(); ++seen) {
            const auto w = nlohmann::json::parse(bus.log[seen], nullptr, false);
            if (!w.is_object() || !w.contains("logos-forum-want")) continue;
            const std::string re = w.value("id", "");
            x.now = t;
            x.send("", nlohmann::json{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", re}, {"more", true},
                                      {"oldest", 1}, {"posts", nlohmann::json::array()}}.dump());
            x.send("", nlohmann::json{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", re}, {"more", true},
                                      {"posts", nlohmann::json::array({oldest_post})}}.dump());
        }
    };
    CHECK(catch_up_over_hours(ea, eb, a, b, sb, 10'000'000, 2, attacker) == 300);
}

TEST(a_replay_of_the_newest_posts_does_not_silence_the_answerers) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum"); a.engine = &ea;
    ea.jitter = [] { return uint64_t{1000}; };
    Account alice{"alice", Keypair::generate()};
    std::vector<std::string> newest;
    const std::string big(2000, 'x');
    for (int i = 0; i < 100; ++i) {
        Post p = topic("t" + std::to_string(i), big); p.ts_ms = 1000 + i;
        const Post sp = author_as(alice, p); sa.put(sp, 1);
        if (i >= 97) newest.push_back(encode(sp));
    }
    const size_t before = bus.log.size();
    ea.receive(R"({"logos-forum-want":1,"forum":"Logos Forum","have":0,"via":"delivery","since":0,"id":"r1"})", 10);
    ea.receive(nlohmann::json{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", "r1"}, {"posts", newest}}.dump(), 20);
    ea.tick(2000); ea.pump(2000);
    bool answered = false;
    for (size_t i = before; i < bus.log.size(); ++i) answered = answered || bus.log[i].find("\"re\":\"r1\"") != std::string::npos;
    CHECK(answered);
}

TEST(a_storage_request_is_answered_over_delivery_by_a_private_node) {
    // A node that does not answer over Storage (it would announce its storage
    // address) still answers, with bundles, so nobody is left without history.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()};
    sa.put(author_as(alice, topic("history")), 1);
    eb.request_history(100);                                     // a Storage-path request
    ea.tick(100); ea.pump(100);
    CHECK(sb.count() == 1);
}

TEST(a_post_dated_in_the_future_is_refused) {
    Bus bus; FakeNet a(bus); Store sa(":memory:");
    Engine ea(sa, a, "Logos Forum");
    const uint64_t now = 1790000000000;
    Post p = topic("from 2036"); p.ts_ms = now + 10ull * 365 * 86400000;
    CHECK(!ea.receive(encode(author_anonymously(p)), now));
    Post q = topic("slightly fast clock"); q.ts_ms = now + 60000;
    CHECK(ea.receive(encode(author_anonymously(q)), now));      // a minute of skew is fine
    // One kept from before dates were checked cannot poison our own history request.
    Post old = topic("already stored"); old.ts_ms = now + 10ull * 365 * 86400000;
    sa.put(author_anonymously(old), 1);
    Post real = topic("real"); real.ts_ms = now - 5000;
    sa.put(author_anonymously(real), 1);
    ea.request_history(now, true);
    const auto w = nlohmann::json::parse(bus.log.back());
    CHECK(w["since"].get<uint64_t>() <= now);
    for (const auto& b : ea.bundles(0, "r", now + Engine::kMaxClockSkewMs))
        CHECK(b.find("already stored") == std::string::npos);     // and it is never sent on
}

TEST(an_unconfirmed_post_is_resent_less_and_less_often) {
    // A post the network accepts but never confirms is not resent every two
    // minutes forever: the wait doubles, up to an hour.
    struct Silent : Transport {
        int sends = 0;
        SendResult send(const std::string&, const std::string&) override { ++sends; return {true, "", "r", false}; }
        bool subscribe(const std::string&) override { return true; }
        std::vector<std::string> history(const std::string&) override { return {}; }
    } net;
    Store s(":memory:");
    Engine e(s, net, "Logos Forum");
    Account alice{"alice", Keypair::generate()};
    e.post_topic(&alice, "t", "b", "", 0);
    for (uint64_t t = 0; t < 24 * 3600000ull; t += 60000) e.pump(t);
    CHECK(net.sends < 40);                                       // not 720
    CHECK(s.outbox().size() == 1);                               // and still kept
}

TEST(snapshot_chunks_never_cut_a_character) {
    const std::string doc = "aaaaaaaaaa\xC3\xA9" "bbbbbbbbbb";   // an é across byte 11
    std::string rebuilt;
    for (size_t off = 0; off < doc.size();) {
        const size_t n = utf8_cut(doc, off, 11);
        CHECK(n > 0);
        rebuilt += doc.substr(off, n);
        off += n;
    }
    CHECK(rebuilt == doc);
    CHECK(utf8_cut(doc, 0, 11) == 10);                           // stops before the é, not inside it
    CHECK(utf8_cut("\xE2\x82\xAC", 0, 2) == 2);                  // one character longer than a chunk: best effort
}

TEST(only_an_https_link_can_be_opened) {
    CHECK(is_openable_link("https://docs.logos.co/run-a-node"));
    CHECK(is_openable_link("HTTPS://a.org"));
    CHECK(is_openable_link("https://a.org/p?x=1&y=2#f"));
    CHECK(is_openable_link("https://127.0.0.1:8979/x"));
    CHECK(is_openable_link("https://\xe4\xbe\x8b\xe3\x81\x88.jp/"));
    for (const char* u : {"http://a.org", "javascript:alert(1)", "file:///etc/passwd", "data:text/html,x", "ftp://a.org",
                          "https://", "https:///x", "https://?q", "https://#f", "https://.a.org", "https://-a",
                          "https://user@a.org", "https://a.org@b.org/", " https://a.org", "https://a.org x",
                          "https://a\"onmouseover=x", "https://a'x", "https://a<img>", "https://a\\b", "https://a`b",
                          "https://a\tb", "https://a\nb", "https:a.org", "https:/a.org", ""})
        CHECK(!is_openable_link(u));
    CHECK(!is_openable_link("https://a.org/" + std::string(2048, 'x')));
}

TEST(announced_addresses_must_be_public) {
    CHECK(!is_public_multiaddr("/ip4/127.0.0.1/tcp/20001"));
    CHECK(!is_public_multiaddr("/ip4/192.168.1.175/tcp/20001"));
    CHECK(!is_public_multiaddr("/ip4/10.0.0.5/tcp/1"));
    CHECK(!is_public_multiaddr("/ip4/172.20.0.2/tcp/1"));
    CHECK(!is_public_multiaddr("/ip4/169.254.133.245/tcp/1"));
    CHECK(!is_public_multiaddr("/ip4/100.64.0.1/tcp/1"));
    CHECK(!is_public_multiaddr("/ip6/::1/tcp/1"));
    CHECK(!is_public_multiaddr("/ip6/fe80::1/tcp/1"));
    CHECK(!is_public_multiaddr("/dns4/localhost/tcp/1"));
    CHECK(!is_public_multiaddr("/ip4/999.1.1.1/tcp/1"));
    // Spellings found by review: every one of these reaches this machine or is not an address.
    for (const char* a : {"/dns4/localhost./tcp/1", "/dns4/LOCALHOST/tcp/1", "/dns4/a.localhost/tcp/1",
                          "/dns4/127.0.0.1.nip.io/tcp/1", "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303",
                          "/ip6/0:0:0:0:0:0:0:1/tcp/1", "/ip6/0::1/tcp/1", "/ip6/0:0::0/tcp/1", "/ip6/::127.0.0.1/tcp/1",
                          "/ip6/::ffff:127.0.0.1/tcp/1", "/ip6/64:ff9b::7f00:1/tcp/1", "/ip6/fec0::1/tcp/1",
                          "/ip4/0177.0.0.1/tcp/1", "/ip4/+8.8.8.8/tcp/1", "/ip4/8.8.8.8 /tcp/1", "/ip4/8.8.8/tcp/1",
                          "ip4/8.8.8.8/tcp/1", "/ip4//tcp/1", "",
                          "/ip6/::ffff:0:7f00:1/tcp/1", "/ip6/2002:7f00:1::/tcp/1", "/ip6/100::1/tcp/1",
                          "/ip6/2001:0:4136::1/tcp/1", "/ip4/192.0.0.8/tcp/1", "/ip4/192.0.2.1/tcp/1"})
        CHECK(!is_public_multiaddr(a));
    CHECK(is_public_multiaddr("/ip4/8.8.8.8/tcp/30303"));
    CHECK(is_public_multiaddr("/ip4/8.8.8.8"));
    CHECK(is_public_multiaddr("/ip6/2a01:4f8::1/tcp/1"));
}

TEST(a_large_import_is_one_commit) {
    Store s(":memory:");
    Bus bus; FakeNet a(bus); Engine e(s, a, "Logos Forum");
    Account alice{"alice", Keypair::generate()};
    nlohmann::json doc{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"posts", nlohmann::json::array()}};
    for (int i = 0; i < 500; ++i) doc["posts"].push_back(encode(author_as(alice, topic("t" + std::to_string(i)))));
    int arrived = 0;
    e.on_post = [&](const Post&, const std::string&) { ++arrived; };
    CHECK(e.import_snapshot(doc.dump(), 5) == 500 && arrived == 500 && s.count() == 500);
}

TEST(the_pubsub_topic_follows_autosharding) {
    // Vector computed independently: sha256("logos-forum" "1")[24..32] mod 8 = 6.
    CHECK(pubsub_topic(content_topic("Logos Forum")) == "/waku/2/rs/2/6");
    CHECK(pubsub_topic("/logos-forum/1/anything-else/json") == "/waku/2/rs/2/6");  // the name does not move the shard
    CHECK(pubsub_topic("nonsense").empty());
}

TEST(each_forum_has_its_own_content_topic) {
    CHECK(content_topic("Logos Forum") != content_topic("logos-forum"));
    CHECK(content_topic("Logos Forum").rfind("/logos-forum/1/logos-forum-", 0) == 0);
}

TEST(resolv_conf_name_servers_are_read_in_order) {
    const std::string text =
        "# Generated by NetworkManager\n"
        "search lan\n"
        "nameserver 10.16.0.1\n"
        "  nameserver\t192.168.1.1   # the router\n"
        "; nameserver 9.9.9.9\n"
        "#nameserver 8.8.4.4\n"
        "nameserver 2001:4860:4860::8888\n"
        "options edns0 trust-ad\n"
        "nameserverx 1.2.3.4\n"
        "nameserver\n";
    CHECK((parse_resolv_conf(text) == std::vector<std::string>{"10.16.0.1", "192.168.1.1", "2001:4860:4860::8888"}));
    CHECK(parse_resolv_conf("").empty());
    // systemd-resolved's stub file: the stub itself is a usable server.
    CHECK((parse_resolv_conf("nameserver 127.0.0.53\noptions edns0 trust-ad\nsearch .\n") == std::vector<std::string>{"127.0.0.53"}));
}

TEST(scutil_keeps_the_default_resolvers_not_mdns_or_per_domain_ones) {
    const std::string text =
        "DNS configuration\n\n"
        "resolver #1\n"
        "  search domain[0] : corp.example\n"
        "  nameserver[0] : 10.16.0.1\n"
        "  nameserver[1] : fe80::1%en0\n"
        "  if_index : 14 (en0)\n"
        "  flags    : Request A records\n"
        "  reach    : 0x00020002 (Reachable,Directly Reachable Address)\n\n"
        "resolver #2\n"
        "  domain   : local\n"
        "  options  : mdns\n"
        "  timeout  : 5\n"
        "  order    : 300000\n\n"
        "resolver #3\n"
        "  domain   : vpn.example\n"
        "  nameserver[0] : 172.16.0.53\n\n"
        "DNS configuration (for scoped queries)\n\n"
        "resolver #1\n"
        "  nameserver[0] : 10.16.0.1\n"
        "  nameserver[1] : 192.168.1.1\n"
        "  if_index : 14 (en0)\n";
    CHECK((parse_scutil_dns(text) == std::vector<std::string>{"10.16.0.1", "fe80::1%en0", "10.16.0.1", "192.168.1.1"}));
    CHECK((dns_servers_for_delivery(parse_scutil_dns(text)) ==
           std::vector<std::string>{"10.16.0.1", "192.168.1.1", "1.1.1.1", "1.0.0.1"}));
    CHECK(parse_scutil_dns("No DNS configuration available\n").empty());
}

TEST(only_ip_literals_become_name_servers) {
    for (const char* ok : {"10.16.0.1", "1.1.1.1", "127.0.0.53", "::1", "2001:4860:4860::8888", " 9.9.9.9 "})
        CHECK(dns_server_literal(ok).has_value());
    CHECK(dns_server_literal("2001:4860:4860:0:0:0:0:8888") == std::optional<std::string>("2001:4860:4860::8888"));
    for (const char* bad : {"", "dns.google", "1.1.1", "1.1.1.1.1", "01.1.1.1", "1.1.1.256", "1.1.1.1:53", "0.0.0.0",
                            "224.0.0.251", "255.255.255.255", "::", "fe80::1", "fe80::1%en0", "fec0:0:0:ffff::1",
                            "ff02::fb", "::ffff:1.1.1.1", "[::1]", "-1.1.1.1", "1.1.1.1 8.8.8.8"})
        CHECK(!dns_server_literal(bad).has_value());
}

TEST(the_system_servers_come_first_then_deliverys_own) {
    CHECK((dns_servers_for_delivery({"10.16.0.1"}) == std::vector<std::string>{"10.16.0.1", "1.1.1.1", "1.0.0.1"}));
    // Deduped, invalid ones dropped, at most four from the system, the fallbacks not repeated.
    CHECK((dns_servers_for_delivery({"1.0.0.1", "bogus", "10.0.0.1", "1.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.4"}) ==
           std::vector<std::string>{"1.0.0.1", "10.0.0.1", "10.0.0.2", "10.0.0.3", "1.1.1.1"}));
    // Nothing usable: an empty list, so Delivery keeps its defaults.
    CHECK(dns_servers_for_delivery({}).empty());
    CHECK(dns_servers_for_delivery({"fe80::1%en0", "localhost"}).empty());
    CHECK((split_dns_list(" 10.16.0.1, 192.168.1.1;;2001:db8::1 ") == std::vector<std::string>{"10.16.0.1", "192.168.1.1", "2001:db8::1"}));
    CHECK(split_dns_list(" , ").empty());
}

int main() {
    for (auto& t : all()) {
        const int before = failures;
        t.f();
        ++ran;
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d failure(s)\n", ran, failures);
    return failures ? 1 : 0;
}
