// SPDX-License-Identifier: MIT OR Apache-2.0
// The forum's core rules, each checked by a test named for what it guarantees.
#include "forum/engine.h"
#include "forum/identity.h"
#include "forum/store.h"
#include "forum/post.h"

#include <cstdio>
#include <functional>
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
        return {true, ""};
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
