// SPDX-License-Identifier: MIT OR Apache-2.0
// Private group forums: invite, sealing, and what the wire carries.
// Included by tests.cpp.

namespace group_tests {

// A network that keeps every message with its topic, delivers it to the
// peers subscribed to that topic, and serves it back as history: the bytes
// a transport sees, and nothing else.
struct TopicBus {
    std::vector<std::pair<std::string, std::string>> log;   // (topic, payload)
    std::vector<struct TopicNet*> peers;
};
struct TopicNet : Transport {
    TopicBus& bus; uint64_t now = 0; bool confirmed = true;
    std::map<std::string, std::function<void(const std::string&)>> on;   // topic -> handler
    explicit TopicNet(TopicBus& b) : bus(b) { bus.peers.push_back(this); }
    SendResult send(const std::string& topic, const std::string& payload) override {
        bus.log.emplace_back(topic, payload);
        for (auto* p : bus.peers)
            if (p != this) { auto it = p->on.find(topic); if (it != p->on.end()) it->second(payload); }
        return {true, "", "", confirmed};
    }
    bool subscribe(const std::string&) override { return true; }
    std::vector<std::string> history(const std::string& topic) override {
        std::vector<std::string> out;
        for (const auto& [t, m] : bus.log) if (t == topic) out.push_back(m);
        return out;
    }
};

// One node: the public forum and, if given, one group, as the plugin wires them.
struct Member {
    TopicNet net; Store store{":memory:"};
    Engine pub;
    std::unique_ptr<SealedTransport> sealed;
    std::unique_ptr<Engine> group;
    Member(TopicBus& bus, const Group* g) : net(bus), pub(store, net, "Logos Forum") {
        net.on[pub.topic()] = [this](const std::string& m) { pub.receive(m, net.now); };
        if (!g) return;
        sealed = std::make_unique<SealedTransport>(net, *g);
        group = std::make_unique<Engine>(store, *sealed, g->forum_id());
        group->jitter = [] { return uint64_t{0}; };
        net.on[sealed->topic()] = [this](const std::string& w) { if (auto m = sealed->open(w)) group->receive(*m, net.now); };
    }
};

bool contains(const std::string& hay, const std::string& needle) { return !needle.empty() && hay.find(needle) != std::string::npos; }

} // namespace group_tests

TEST(an_invite_carries_the_secret_and_the_name_and_nothing_else_parses) {
    const auto g = Group::create("Core contributors");
    CHECK(g.has_value());
    const std::string inv = g->invite();
    CHECK(inv.rfind("logosforum://join/", 0) == 0);
    const auto back = Group::from_invite("  " + inv + "\n");
    CHECK(back && back->secret == g->secret && back->name == "Core contributors");
    CHECK(back && back->forum_id() == g->forum_id() && back->content_topic() == g->content_topic());
    CHECK(Group::create("Core contributors")->secret != g->secret);     // a new group, a new secret
    CHECK(!Group::from_invite("logosforum://join/"));
    CHECK(!Group::from_invite("https://join/" + inv.substr(18)));
    CHECK(!Group::from_invite(inv.substr(0, inv.size() - 20)));          // cut short
    CHECK(!Group::from_invite("logosforum://join/!!!!"));
    CHECK(!Group::from_invite("logosforum://join/" + std::string(2000, 'A')));
    CHECK(!Group::create("") && !Group::create("\xE2\x80\x8B") && !Group::create(std::string(kMaxGroupName + 1, 'n')));
    {   // a well-formed invite whose name is one byte too long
        std::string raw(1, '\x01'); raw += std::string(32, '\x07'); raw += std::string(kMaxGroupName + 1, 'n');
        std::string enc(sodium_base64_encoded_len(raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
        sodium_bin2base64(enc.data(), enc.size(), reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        enc.resize(std::strlen(enc.c_str()));
        CHECK(!Group::from_invite("logosforum://join/" + enc));
        raw.resize(raw.size() - 1);
        enc.assign(sodium_base64_encoded_len(raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
        sodium_bin2base64(enc.data(), enc.size(), reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        enc.resize(std::strlen(enc.c_str()));
        CHECK(Group::from_invite("logosforum://join/" + enc).has_value());   // at the limit: fine
    }
    // The name is cleaned like an alias: it cannot reorder what is shown next to it.
    CHECK(Group::create("\xE2\x80\xAE" "evil")->name == "evil");
}

TEST(the_topic_the_key_and_the_forum_id_are_separate_subkeys) {
    const auto g = Group::create("g");
    const SealKey k = g->seal_key();
    const std::string topic = g->content_topic(), id = g->forum_id();
    CHECK(topic.rfind("/logos-forum/1/g-", 0) == 0 && id.rfind("g:", 0) == 0);
    const std::string khex = to_hex(k.data(), k.size()), shex = to_hex(g->secret.data(), 32);
    // Neither the topic nor the forum id contains the key, or the secret.
    for (const auto& s : {topic, id}) for (size_t i = 0; i + 8 <= 64; i += 8) {
        CHECK(!group_tests::contains(s, khex.substr(i, 8)));
        CHECK(!group_tests::contains(s, shex.substr(i, 8)));
    }
    CHECK(topic.substr(17, 32) != id.substr(2, 32));
    CHECK(pubsub_topic(topic, 3) == pubsub_topic(content_topic("Logos Forum"), 3));   // the forum's shard
}

TEST(a_sealed_message_round_trips) {
    const auto g = Group::create("g");
    for (const std::string m : {std::string(), std::string("hello"), std::string(5000, 'x'), std::string(110 * 1024, 'y')}) {
        const std::string w = seal(g->seal_key(), g->content_topic(), m);
        const auto back = open_sealed(g->seal_key(), g->content_topic(), w);
        CHECK(back && *back == m);
    }
    // Two seals of one message differ (a fresh nonce each time).
    CHECK(seal(g->seal_key(), g->content_topic(), "same") != seal(g->seal_key(), g->content_topic(), "same"));
}

TEST(a_wrong_key_cannot_read_a_sealed_message) {
    const auto g = Group::create("g"), other = Group::create("g");   // same name, another secret
    const std::string w = seal(g->seal_key(), g->content_topic(), "the plan");
    CHECK(!open_sealed(other->seal_key(), g->content_topic(), w));
    CHECK(!open_sealed(other->seal_key(), other->content_topic(), w));
    // Nor is a message accepted on another group's topic, even if replayed there.
    CHECK(!open_sealed(g->seal_key(), other->content_topic(), w));
}

TEST(a_tampered_sealed_message_is_rejected) {
    const auto g = Group::create("g");
    const std::string w = seal(g->seal_key(), g->content_topic(), "pay alice 10");
    size_t n = 0;
    std::string raw(w.size(), '\0');
    sodium_base642bin(reinterpret_cast<unsigned char*>(raw.data()), raw.size(), w.data(), w.size(), nullptr, &n, nullptr, sodium_base64_VARIANT_ORIGINAL);
    raw.resize(n);
    auto rewrap = [](const std::string& r) {
        std::string out(sodium_base64_encoded_len(r.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
        sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(r.data()), r.size(), sodium_base64_VARIANT_ORIGINAL);
        out.resize(std::strlen(out.c_str()));
        return out;
    };
    CHECK(open_sealed(g->seal_key(), g->content_topic(), rewrap(raw)).has_value());   // the rewrap itself is faithful
    for (size_t at : {size_t{0}, size_t{1}, size_t{30}, raw.size() / 2, raw.size() - 1}) {
        std::string t = raw; t[at] ^= 0x01;
        CHECK(!open_sealed(g->seal_key(), g->content_topic(), rewrap(t)));
    }
    CHECK(!open_sealed(g->seal_key(), g->content_topic(), rewrap(raw.substr(0, raw.size() - 1))));
    CHECK(!open_sealed(g->seal_key(), g->content_topic(), "not base64 at all"));
    CHECK(!open_sealed(g->seal_key(), g->content_topic(), std::string(kMaxSealedBytes + 4, 'A')));
}

TEST(the_public_topic_never_carries_a_group_post) {
    // One store, two forums, as in the plugin: the public engine and a
    // group's. Its outbox is shared, so the public engine must leave the
    // group's posts to the group, even when it pumps first.
    using namespace group_tests;
    const auto g = Group::create("g");
    TopicBus bus; Member a(bus, &*g);
    a.net.confirmed = false;   // posts stay in the outbox until confirmed: every pump sees them
    Account me{"me", Keypair::generate()};
    const std::string gid = a.group->post_topic(&me, "group secret title", "group secret body", "", 1000);
    const std::string pid = a.pub.post_topic(&me, "public title", "public body", "", 1000);
    for (uint64_t t = 1000; t < 400'000; t += 130'000) { a.pub.pump(t); a.group->pump(t); a.pub.reconnected(t); }
    size_t on_public = 0, on_group = 0;
    for (const auto& [topic, m] : bus.log) {
        if (topic == a.pub.topic()) {
            ++on_public;
            const auto p = decode(m);
            CHECK(p && p->id() == pid);
            CHECK(!contains(m, "group secret"));
        } else {
            CHECK(topic == a.sealed->topic());
            ++on_group;
        }
    }
    CHECK(on_public > 0 && on_group > 0);
    CHECK(a.store.get(gid)->forum == g->forum_id());
}

TEST(nothing_of_a_group_is_in_plaintext_on_the_wire) {
    // Posts in every mode, a history request and the bundles that answer it,
    // as the transport sees them: no title, body, alias, group name, forum
    // id, author key or message tag, and nothing that parses as JSON.
    using namespace group_tests;
    const auto g = Group::create("Night shift");
    TopicBus bus; Member a(bus, &*g), b(bus, nullptr), c(bus, &*g);
    Account alice{"alice", Keypair::generate()};
    const std::string t = a.group->post_topic(&alice, "Quarterly numbers", "Revenue is up 12 percent", "", 1000);
    a.group->post_reply(&alice, t, "Signed under an alias", "Treasurer", 1001);
    a.group->post_reply(nullptr, t, "And one anonymous", "", 1002);
    a.group->pump(1003);
    // A member who was away asks for history; the answer comes as bundles.
    Store fresh(":memory:"); TopicNet dn(bus); SealedTransport ds(dn, *g); Engine d(fresh, ds, g->forum_id());
    dn.on[ds.topic()] = [&](const std::string& w) { if (auto m = ds.open(w)) d.receive(*m, 5000); };
    d.request_history(2000, true);
    a.group->tick(5000); a.group->pump(5000);
    CHECK(fresh.count() == 3);
    std::vector<std::string> secrets = {"Quarterly", "Revenue", "Treasurer", "anonymous", "Night shift", g->forum_id(),
                                        g->forum_id().substr(2), to_hex(alice.key.pk.data(), 32),
                                        "logos-forum-want", "logos-forum-snapshot", "\"forum\"", "\"sig\""};
    {   // the author key as it appears in a post, base64
        std::string k(sodium_base64_encoded_len(32, sodium_base64_VARIANT_ORIGINAL), '\0');
        sodium_bin2base64(k.data(), k.size(), alice.key.pk.data(), 32, sodium_base64_VARIANT_ORIGINAL);
        secrets.push_back(k.substr(0, 20));
    }
    size_t group_messages = 0;
    for (const auto& [topic, m] : bus.log) {
        if (topic != a.sealed->topic()) continue;
        ++group_messages;
        for (const auto& s : secrets) CHECK(!contains(m, s));
        CHECK(nlohmann::json::parse(m, nullptr, false).is_discarded());
        CHECK(!decode(m).has_value());
    }
    CHECK(group_messages >= 5);   // three posts, a request, at least one bundle
    CHECK(b.store.count() == 0);  // a public-only node on the same network stored nothing
}

TEST(sealed_sizes_say_little_about_the_post) {
    const auto g = Group::create("g");
    const size_t a = seal(g->seal_key(), g->content_topic(), std::string(10, 'x')).size();
    const size_t b = seal(g->seal_key(), g->content_topic(), std::string(900, 'x')).size();
    CHECK(a == b);
}

TEST(a_member_catches_up_from_store_nodes_and_from_members_and_an_outsider_reads_nothing) {
    using namespace group_tests;
    const auto g = Group::create("g"), other = Group::create("g");
    TopicBus bus; Member a(bus, &*g);
    Account alice{"alice", Keypair::generate()};
    for (int i = 0; i < 5; ++i) a.group->post_topic(&alice, "t" + std::to_string(i), "b", "", 1000 + i);
    a.group->pump(2000);
    // From the network's store: what is on the group's topic, opened.
    Member late(bus, &*g);
    CHECK(late.group->catch_up(3000) == 5);
    // From a member, as sealed bundles.
    Member later(bus, &*g);
    later.group->request_history(4000, true);
    a.group->tick(4000); a.group->pump(60'000);   // the answer is paced like any send
    CHECK(later.store.count() == 5);
    // Someone holding another group's invite, on the same topic, gets nothing.
    TopicNet xn(bus); SealedTransport xs(xn, *other); Store xstore(":memory:"); Engine x(xstore, xs, other->forum_id());
    for (const auto& [topic, m] : bus.log) if (topic == g->content_topic()) { if (auto p = xs.open(m)) x.receive(*p, 5000); }
    CHECK(xstore.count() == 0);
    // And the public forum engine ignores the group's posts even in plaintext.
    for (const auto& p : a.store.all(g->forum_id())) CHECK(!a.pub.receive(encode(p), 6000));
    CHECK(a.store.count("Logos Forum") == 0);
}

TEST(leaving_a_group_deletes_its_secret_and_posts_from_the_device) {
    using namespace group_tests;
    Store s(":memory:"); Groups gs(s);
    const auto g = Group::create("g"), h = Group::create("h");
    CHECK(gs.save(*g, 1) && gs.save(*h, 2) && !gs.save(*g, 3));
    CHECK(gs.all().size() == 2 && gs.all()[0].secret == g->secret && gs.all()[0].name == "g");
    TopicBus bus; TopicNet n(bus); n.confirmed = false; SealedTransport st(n, *g); Engine e(s, st, g->forum_id());
    Engine pub(s, n, "Logos Forum");
    Account me{"me", Keypair::generate()};
    e.post_topic(&me, "in g", "b", "", 10);
    const std::string kept = pub.post_topic(&me, "public", "b", "", 10);
    CHECK(s.outbox().size() == 2);
    CHECK(gs.remove(g->forum_id()));
    CHECK(s.count(g->forum_id()) == 0 && s.outbox().size() == 1 && s.outbox()[0].id == kept);
    CHECK(gs.all().size() == 1 && gs.all()[0].secret == h->secret);
    CHECK(!gs.remove(g->forum_id()));
}
