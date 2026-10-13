// SPDX-License-Identifier: MIT OR Apache-2.0
// Moderation for the reader: local, per account, by key. Included by tests.cpp.

TEST(muting_is_by_key_so_a_shared_alias_hides_only_the_muted_author) {
    Bus bus; FakeNet a(bus); Store s(":memory:"); Engine e(s, a, "Logos Forum"); a.engine = &e;
    Moderation mod(s);
    Account troll{"troll", Keypair::generate()}, bob{"bob", Keypair::generate()};
    const Post t1 = author_as(troll, topic("by the troll"));
    const Post t2 = author_as(troll, topic("by the troll, as Ghost"), "Ghost");
    const Post t3 = author_as(bob, topic("by bob, as Ghost too"), "Ghost");
    for (const auto& p : {t1, t2, t3}) s.put(p, 1);
    Post r; r.kind = Kind::Reply; r.forum = "Logos Forum"; r.topic_id = t3.id(); r.body = "noise";
    s.put(author_as(troll, r, "Ghost"), 2);
    r.body = "a real answer"; s.put(author_as(bob, r), 3);
    CHECK(mod.mute("Account 1", troll.key.pk, 10));
    const auto topics = mod.visible("Account 1", s.topics("Logos Forum"));
    CHECK(topics.size() == 1 && topics[0].id == t3.id());          // bob's "Ghost" stays
    const auto replies = mod.visible("Account 1", s.replies(t3.id()));
    CHECK(replies.size() == 1 && replies[0].body == "a real answer");
    CHECK(s.count() == 5);                                         // hidden from view, still stored
}

TEST(hiding_a_topic_or_a_reply_hides_only_it) {
    Store s(":memory:"); Moderation mod(s);
    Account alice{"alice", Keypair::generate()};
    const Post t1 = author_as(alice, topic("one")), t2 = author_as(alice, topic("two"));
    s.put(t1, 1); s.put(t2, 1);
    Post r; r.kind = Kind::Reply; r.forum = "Logos Forum"; r.topic_id = t1.id();
    r.body = "keep"; const Post keep = author_as(alice, r); s.put(keep, 2);
    r.body = "hide"; const Post gone = author_as(alice, r); s.put(gone, 2);
    CHECK(mod.hide("me", t2.id(), 5));
    CHECK(mod.hide("me", gone.id(), 6));
    CHECK(!mod.hide("me", "not an id", 7));
    const auto topics = mod.visible("me", s.topics("Logos Forum"));
    CHECK(topics.size() == 1 && topics[0].id == t1.id());
    const auto replies = mod.visible("me", s.replies(t1.id()));
    CHECK(replies.size() == 1 && replies[0].id() == keep.id());
    CHECK(mod.unhide("me", t2.id()));
    CHECK(mod.visible("me", s.topics("Logos Forum")).size() == 2);
}

TEST(the_muted_list_shows_keys_and_unmute_brings_posts_back) {
    Store s(":memory:"); Moderation mod(s);
    Account x{"x", Keypair::generate()}, y{"y", Keypair::generate()};
    s.put(author_as(x, topic("x")), 1);
    CHECK(mod.mute("me", x.key.pk, 10) && mod.mute("me", y.key.pk, 20));
    CHECK(!mod.mute("me", x.key.pk, 30));          // already muted
    const auto list = mod.muted("me");
    CHECK(list.size() == 2 && list[0].key == y.key.pk && list[1].key == x.key.pk);
    CHECK(mod.visible("me", s.topics("Logos Forum")).empty());
    CHECK(mod.unmute("me", x.key.pk) && !mod.is_muted("me", x.key.pk));
    CHECK(mod.visible("me", s.topics("Logos Forum")).size() == 1);
}

TEST(moderation_is_per_account_and_kept_across_a_restart) {
    const std::string path = "/tmp/forum-moderation-test-" + std::to_string(::getpid()) + ".db";
    std::remove(path.c_str());
    Account x{"x", Keypair::generate()};
    std::string id;
    {
        Store s(path); Moderation mod(s);
        const Post t = author_as(x, topic("t")); id = t.id(); s.put(t, 1);
        mod.mute("Account 1", x.key.pk, 1);
        mod.hide("Account 1", id, 1);
    }
    {
        Store s(path); Moderation mod(s);
        CHECK(mod.is_muted("Account 1", x.key.pk) && mod.is_hidden("Account 1", id));
        CHECK(!mod.is_muted("Account 2", x.key.pk) && !mod.is_hidden("Account 2", id));
        CHECK(mod.visible("Account 2", s.topics("Logos Forum")).size() == 1);
        mod.forget("Account 1");                     // the account was deleted
        CHECK(mod.muted("Account 1").empty() && mod.hidden("Account 1").empty());
    }
    for (const char* side : {"", "-wal", "-shm"}) std::remove((path + side).c_str());
}

TEST(muting_and_hiding_are_never_sent_or_shared) {
    // Nothing goes on the network, and what this node serves others (its
    // snapshot, its history answers) is exactly what it was before.
    Bus bus; FakeNet a(bus), b(bus); Store sa(":memory:"), sb(":memory:");
    Engine ea(sa, a, "Logos Forum"), eb(sb, b, "Logos Forum"); a.engine = &ea; b.engine = &eb;
    ea.jitter = [] { return uint64_t{0}; };
    Account alice{"alice", Keypair::generate()}, troll{"troll", Keypair::generate()};
    const Post t = author_as(troll, topic("loud")); sa.put(t, 1);
    sa.put(author_as(alice, topic("quiet")), 1);
    const std::string before = ea.snapshot();
    const size_t sent = bus.log.size();
    Moderation mod(sa);
    mod.mute("alice", troll.key.pk, 5);
    mod.hide("alice", t.id(), 5);
    ea.pump(10); ea.tick(10);
    CHECK(bus.log.size() == sent);
    CHECK(ea.snapshot() == before);
    eb.request_history(100, true);
    ea.tick(100); ea.pump(100);
    CHECK(sb.has(t.id()));                            // a newcomer still gets the muted author's post
    for (const auto& m : bus.log) CHECK(m.find("muted") == std::string::npos && m.find("hidden") == std::string::npos);
}
