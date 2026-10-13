// SPDX-License-Identifier: MIT OR Apache-2.0
// Built with the 0.3.6 headers and `forum` defined as `forum_v036`.
#include "compat_v036.h"

#include "forum/engine.h"
#include "forum/post.h"
#include "forum/store.h"

#include <nlohmann/json.hpp>

namespace v036 {

bool decode(const std::string& payload, std::string* body, std::string* id) {
    const auto p = forum::decode(payload);
    if (!p) return false;
    if (body) *body = p->body;
    if (id) *id = p->id();
    return true;
}

namespace {
struct Recorder : forum::Transport {
    std::vector<std::string> sent;
    forum::SendResult send(const std::string&, const std::string& payload) override {
        sent.push_back(payload);
        return {true, "", "", true};
    }
    bool subscribe(const std::string&) override { return true; }
    std::vector<std::string> history(const std::string&) override { return {}; }
};
} // namespace

Outcome receive(const std::vector<std::string>& payloads, uint64_t now_ms) {
    forum::Store store(":memory:");
    Recorder net;
    forum::Engine engine(store, net, "Logos Forum");
    Outcome o;
    for (const auto& m : payloads) {
        if (engine.receive(m, now_ms)) ++o.accepted;
        engine.tick(now_ms + 600000);
        engine.pump(now_ms + 600000);
    }
    o.stored = store.count();
    o.sent = net.sent;
    for (const auto& p : store.all("Logos Forum")) o.bodies.push_back(p.body);
    const auto snap = nlohmann::json::parse(engine.snapshot(), nullptr, false);
    if (snap.is_object() && snap.contains("posts"))
        for (const auto& p : snap["posts"]) o.relayed.push_back(p.get<std::string>());
    return o;
}

} // namespace v036
