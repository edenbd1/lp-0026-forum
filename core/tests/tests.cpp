// SPDX-License-Identifier: MIT OR Apache-2.0
// The forum's core rules, each checked by a test named for what it guarantees.
#include "forum/identity.h"
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
