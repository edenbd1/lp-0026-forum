// SPDX-License-Identifier: MIT OR Apache-2.0
// The forum's core rules, each checked by a test named for what it guarantees.
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
    ea.announce_snapshot("zDvZRwzkwWtSfgKFoPMRaxzAAJ1i", 7, 10);
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
    CHECK(sb.count() == 300);                                   // everything arrived
    size_t bundles = 0, largest = 0;
    for (size_t i = before; i < bus.log.size(); ++i)
        if (bus.log[i].find("logos-forum-snapshot\"") != std::string::npos) { ++bundles; largest = std::max(largest, bus.log[i].size()); }
    CHECK(bundles >= 3 && largest < 150 * 1024);               // split under the network's limit
    // A forged post inside a bundle is dropped like any other.
    Post forged = author_as(alice, topic("forged")); forged.body = "edited";
    auto doc = nlohmann::json::parse(ea.bundles(0, "r")[0]);
    doc["posts"].push_back(encode(forged));
    CHECK(!eb.receive(doc.dump(), 6000000) && sb.count() == 300);
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
    ea.tick(5000);
    CHECK(sb.count() == 1);                                     // the Delivery answer still came
    eb.request_history(6000, true);                             // and the per-path limit still holds
    ea.tick(6000);
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
    ea.tick(10'000'100);
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
    for (const char* m : {
             R"({"logos-forum-snapshot-at":"1","forum":"Logos Forum","cid":"x"})",
             R"({"logos-forum-snapshot-at":1,"forum":5,"cid":"x"})",
             R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":null})",
             R"({"logos-forum-snapshot-at":1,"forum":"Logos Forum","cid":"x","posts":"many"})",
             R"({"v":"1","kind":7})", "[]", "null", "", "{"}) {
        CHECK(!ea.receive(m, 1));
    }
    CHECK(calls == 1);  // only the last well-formed pointer, with a bad count read as 0
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":"1","forum":"Logos Forum","posts":[]})", 1) == 0);
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":1,"forum":null,"posts":[]})", 1) == 0);
    CHECK(ea.import_snapshot(R"({"logos-forum-snapshot":1,"forum":"Logos Forum","posts":{"a":1}})", 1) == 0);
    CHECK(sa.count() == 0);
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
