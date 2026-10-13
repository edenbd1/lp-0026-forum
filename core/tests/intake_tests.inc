// SPDX-License-Identifier: MIT OR Apache-2.0
// Receive-side flood control: what a node agrees to store from the network.
// Included by tests.cpp.

namespace intake_tests {

constexpr uint64_t kNow = 1'790'000'000'000;   // a real clock: the date checks are on
constexpr uint64_t kHour = 3'600'000;

Post topic_at(const std::string& title, uint64_t ts) {
    Post p; p.kind = Kind::Topic; p.forum = "Logos Forum"; p.title = title; p.body = "b"; p.ts_ms = ts; return p;
}
Post reply_at(const std::string& to, const std::string& body, uint64_t ts) {
    Post p; p.kind = Kind::Reply; p.forum = "Logos Forum"; p.topic_id = to; p.body = body; p.ts_ms = ts; return p;
}
// Makes a key known to the store (it holds a post by it), without the intake.
void make_known(Store& s, Account& a) { s.put(author_as(a, topic_at("hello from " + a.label, kNow - 10 * kHour)), 1); }

struct Node {
    Bus bus; FakeNet net{bus}; Store store{":memory:"}; Engine engine{store, net, "Logos Forum"};
    Node() { net.engine = &engine; }
    bool live(const Post& p, uint64_t at) { return engine.receive(encode(p), at); }
};

} // namespace intake_tests

TEST(an_author_key_gets_a_bounded_number_of_topics_an_hour) {
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    int stored = 0;
    for (int i = 0; i < 30; ++i) stored += n.live(author_as(alice, topic_at("t" + std::to_string(i), kNow + i)), kNow + i);
    CHECK(stored == 20);
    CHECK(n.engine.intake().refused(Intake::Verdict::KeyTopics) == 10);
    // An hour of arrival time later, the window has room again.
    CHECK(n.live(author_as(alice, topic_at("later", kNow + kHour + 100)), kNow + kHour + 100));
}

TEST(topics_and_replies_are_counted_apart_per_key) {
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    for (int i = 0; i < 20; ++i) n.live(author_as(alice, topic_at("t" + std::to_string(i), kNow)), kNow);
    CHECK(!n.live(author_as(alice, topic_at("one too many", kNow)), kNow));
    const std::string t = n.store.topics("Logos Forum").front().id;
    int replies = 0;
    for (int i = 0; i < 130; ++i) replies += n.live(author_as(alice, reply_at(t, "r" + std::to_string(i), kNow)), kNow);
    CHECK(replies == 120);   // the topic budget being spent takes nothing from the replies
    CHECK(n.engine.intake().refused(Intake::Verdict::KeyReplies) == 10);
    // Another key is not affected by alice's.
    Account bob{"bob", Keypair::generate()}; make_known(n.store, bob);
    CHECK(n.live(author_as(bob, topic_at("bob's", kNow)), kNow));
}

TEST(brand_new_keys_share_one_budget) {
    // Anonymous posts are signed by a key made for each: per-key limits do not
    // hold them, the shared budget for keys never seen does.
    using namespace intake_tests;
    Node n;
    int stored = 0;
    for (int i = 0; i < 100; ++i) stored += n.live(author_anonymously(topic_at("anon" + std::to_string(i), kNow)), kNow + i);
    CHECK(stored == 60);
    CHECK(n.engine.intake().refused(Intake::Verdict::NewKeys) == 40);
    // A key this node already holds a post from is not held back by them.
    Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    CHECK(n.live(author_as(alice, topic_at("still heard", kNow + 200)), kNow + 200));
    // The shared budget refills with arrival time.
    CHECK(n.live(author_anonymously(topic_at("an hour on", kNow)), kNow + kHour + 1));
}

TEST(everything_live_has_an_hourly_total_whatever_the_number_of_keys) {
    using namespace intake_tests;
    Node n;
    IntakeLimits l; l.live_posts = 50; n.engine.set_intake_limits(l);
    std::vector<Account> keys;
    for (int i = 0; i < 30; ++i) { keys.push_back({"k" + std::to_string(i), Keypair::generate()}); make_known(n.store, keys.back()); }
    int stored = 0;
    for (int round = 0; round < 3; ++round)
        for (auto& k : keys) stored += n.live(author_as(k, topic_at("t" + std::to_string(round), kNow)), kNow + round);
    CHECK(stored == 50);
    CHECK(n.engine.intake().refused(Intake::Verdict::LiveTotal) == 40);
    CHECK(n.engine.intake().tracked_keys() <= 50);
}

TEST(back_dated_or_reordered_posts_cannot_escape_the_budget) {
    // The budget follows when a post ARRIVES. Dates a year back, dates in
    // reverse order, dates an hour apart: all arrive in the same hour, so
    // they share one window.
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    int stored = 0;
    for (int i = 0; i < 40; ++i) {
        const uint64_t ts = i % 2 ? kNow - uint64_t(i) * kHour : kNow - 365 * 24 * kHour + uint64_t(40 - i) * kHour;
        stored += n.live(author_as(alice, topic_at("t" + std::to_string(i), ts)), kNow + i * 1000);
    }
    CHECK(stored == 20);
    // A post dated two hours ago is still charged now: no "it belongs to an old window".
    CHECK(!n.live(author_as(alice, topic_at("dated before the window", kNow - 2 * kHour)), kNow + 50'000));
    // And the clock of this node stepping back does not reopen the window.
    CHECK(!n.live(author_as(alice, topic_at("clock went back", kNow - 3 * kHour)), kNow - 3 * kHour));
    CHECK(n.live(author_as(alice, topic_at("an hour of arrival later", kNow)), kNow + 40'000 + kHour));
}

TEST(a_copy_of_a_stored_post_costs_no_budget) {
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    const Post p = author_as(alice, topic_at("once", kNow));
    CHECK(n.live(p, kNow));
    for (int i = 0; i < 100; ++i) n.live(p, kNow + i);   // replayed a hundred times
    int stored = 0;
    for (int i = 0; i < 19; ++i) stored += n.live(author_as(alice, topic_at("t" + std::to_string(i), kNow)), kNow + 200);
    CHECK(stored == 19);   // the replays took nothing from alice's 20
}

TEST(a_bundle_nobody_asked_for_is_charged_like_live_posts) {
    // Anyone can send a bundle. One that answers no open request of ours does
    // not get the history budget: its posts are charged as if they arrived live.
    using namespace intake_tests;
    Node n;
    nlohmann::json doc{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", "not-ours"}, {"posts", nlohmann::json::array()}};
    for (int i = 0; i < 200; ++i) doc["posts"].push_back(encode(author_anonymously(topic_at("junk" + std::to_string(i), kNow - 5 * kHour))));
    n.engine.receive(doc.dump(), kNow);
    CHECK(n.store.count() == 60);
    CHECK(n.engine.intake().refused(Intake::Verdict::NewKeys) == 140);
}

TEST(history_has_its_own_budget_so_a_backlog_still_arrives) {
    // The live budgets are spent, and a store node's archive of 1,500 posts by
    // one author (a real backlog) still comes in, under the history budget.
    using namespace intake_tests;
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea;
    Account alice{"alice", Keypair::generate()};
    for (int i = 0; i < 1500; ++i) bus.log.push_back(encode(author_as(alice, topic_at("t" + std::to_string(i), kNow - 30 * kHour + i))));
    for (int i = 0; i < 100; ++i) eb.receive(encode(author_anonymously(topic_at("flood" + std::to_string(i), kNow))), kNow);
    CHECK(eb.intake().refused(Intake::Verdict::NewKeys) == 40);
    CHECK(eb.catch_up(kNow + 1) == 1500);
    CHECK(sb.count() == 1560);
}

TEST(history_is_bounded_too) {
    // A peer answering our request (or a store node archiving a flood) cannot
    // make this node store more than the history budget in an hour.
    using namespace intake_tests;
    Bus bus; FakeNet b(bus); Store sb(":memory:");
    Engine eb(sb, b, "Logos Forum");
    IntakeLimits l; l.history_posts = 100; eb.set_intake_limits(l);
    for (int i = 0; i < 300; ++i) bus.log.push_back(encode(author_anonymously(topic_at("old" + std::to_string(i), kNow - kHour))));
    CHECK(eb.catch_up(kNow) == 100);
    CHECK(eb.intake().refused(Intake::Verdict::HistoryPosts) == 200);
    CHECK(eb.request_history(kNow + 10, true).empty());   // no request while there is no room
    CHECK(!eb.request_history(kNow + kHour, true).empty());  // an hour later there is room
    CHECK(eb.catch_up(kNow + kHour) == 100);                // and it takes the next part
    // In bytes as well as in posts.
    Bus bus2; FakeNet c(bus2); Store sc(":memory:"); Engine ec(sc, c, "Logos Forum");
    IntakeLimits lb; lb.history_bytes = 20'000; ec.set_intake_limits(lb);
    const std::string big(4000, 'x');
    for (int i = 0; i < 10; ++i) { Post p = topic_at("big" + std::to_string(i), kNow - kHour); p.body = big; bus2.log.push_back(encode(author_anonymously(p))); }
    ec.catch_up(kNow);
    CHECK(sc.count() == 4);
    CHECK(ec.intake().refused(Intake::Verdict::HistoryBytes) > 0);
}

TEST(posts_the_history_budget_refused_are_not_paged_past) {
    // A backlog bigger than the history budget arrives in full over the hours:
    // a page cut short by the budget resumes above its first refused post.
    using namespace intake_tests;
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    IntakeLimits l; l.history_posts = 150; eb.set_intake_limits(l);
    Account alice{"alice", Keypair::generate()};
    const std::string body(1000, 'x');
    for (int i = 0; i < 600; ++i) { Post p = topic("t" + std::to_string(i), body); p.ts_ms = 1'000'000 + i * 1000; sa.put(author_as(alice, p), 1); }
    CHECK(catch_up_over_hours(ea, eb, a, b, sb, 10'000'000, 6) == 600);
}

TEST(an_oversized_payload_is_dropped_before_it_is_parsed) {
    // A valid post padded with fields the decoder ignores would be accepted at
    // any size; above the payload cap it is not even parsed.
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()};
    nlohmann::json j = nlohmann::json::parse(encode(author_as(alice, topic_at("padded", kNow))));
    j["pad"] = std::string(Engine::kMaxPayloadBytes, 'x');
    CHECK(!n.engine.receive(j.dump(), kNow));
    j["pad"] = std::string(1000, 'x');
    CHECK(n.engine.receive(j.dump(), kNow));   // the same post, small enough
}

TEST(titles_aliases_and_bodies_over_their_caps_are_refused_on_receipt) {
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()};
    CHECK(!n.live(author_as(alice, topic_at(std::string(kMaxTitle + 1, 't'), kNow)), kNow));
    CHECK(!n.live(author_as(alice, topic_at("t", kNow), std::string(kMaxAlias + 1, 'a')), kNow));
    Post big = topic_at("t", kNow); big.body = std::string(kMaxBody + 1, 'b');
    CHECK(!n.live(author_as(alice, big), kNow));
    CHECK(n.store.count() == 0);
    CHECK(n.engine.intake().refused() == 0);   // refused as malformed, before any budget
}

TEST(a_clock_stepping_back_does_not_widen_the_budget) {
    // Ten topics now, the clock steps back an hour, ten more, the clock comes
    // back: the ten from the "past" were charged now, so none is left.
    using namespace intake_tests;
    Node n; Account alice{"alice", Keypair::generate()}; make_known(n.store, alice);
    int stored = 0;
    for (int i = 0; i < 10; ++i) stored += n.live(author_as(alice, topic_at("a" + std::to_string(i), kNow)), kNow);
    for (int i = 0; i < 10; ++i) stored += n.live(author_as(alice, topic_at("b" + std::to_string(i), kNow)), kNow - kHour + 1);
    for (int i = 0; i < 10; ++i) stored += n.live(author_as(alice, topic_at("c" + std::to_string(i), kNow)), kNow + 1);
    CHECK(stored == 20);
}

TEST(a_page_cut_short_by_the_budget_resumes_above_its_first_refused_post) {
    // Our request is answered with ten posts, newest first, and "more"; the
    // history budget has room for five. The next page must start after the
    // fifth (the oldest one stored), not after the tenth.
    using namespace intake_tests;
    Bus bus; FakeNet b(bus); Store sb(":memory:"); Engine eb(sb, b, "Logos Forum"); b.engine = &eb;
    IntakeLimits l; l.history_posts = 5; eb.set_intake_limits(l);
    const std::string re = eb.request_history(1'000'000, true);
    Account alice{"alice", Keypair::generate()};
    std::vector<Post> posts;
    for (int i = 0; i < 10; ++i) posts.push_back(author_as(alice, topic("t" + std::to_string(i))));
    for (int i = 0; i < 10; ++i) { posts[i].ts_ms = 900'000 - i * 1000; sign(posts[i], alice.key.sk); }
    nlohmann::json doc{{"logos-forum-snapshot", 1}, {"forum", "Logos Forum"}, {"re", re}, {"more", true}, {"posts", nlohmann::json::array()}};
    for (const auto& p : posts) doc["posts"].push_back(encode(p));
    eb.receive(doc.dump(), 1'000'100);
    CHECK(sb.count() == 5);
    eb.tick(1'000'100 + 70'000);                       // no room yet: the page waits
    CHECK(nlohmann::json::parse(bus.log.back()).value("id", "") == re);
    eb.tick(1'000'100 + 3'600'000);                    // an hour on: it is asked for
    const auto want = nlohmann::json::parse(bus.log.back());
    CHECK(want.value("until", uint64_t{0}) == posts[4].ts_ms);
    CHECK(want.value("until_id", std::string()) == posts[4].id());
}
