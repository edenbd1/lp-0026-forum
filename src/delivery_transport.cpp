// SPDX-License-Identifier: MIT OR Apache-2.0
#include "delivery_transport.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QString>
#include <QUuid>
#include <QVariant>

#include <nlohmann/json.hpp>

#include <sodium.h>

#include "logos_sdk.h"
#include "logos_types.h"

using json = nlohmann::json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// LogosResult's accessors throw when read against the wrong outcome, so every
// read goes through these.
std::string error_of(const LogosResult& r) { return r.success ? std::string() : r.getError().toStdString(); }
std::string string_of(const LogosResult& r) { return r.success ? r.getString().toStdString() : std::string(); }

std::string b64decode(const std::string& in) {
    std::string out(in.size(), '\0');
    size_t n = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), in.data(), in.size(),
                          " \n", &n, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        return {};
    out.resize(n);
    return out;
}

constexpr int kHistoryTimeoutMs = 8000;
constexpr int kHistoryPageLimit = 100;
constexpr int kHistoryMaxPages = 20;

} // namespace

DeliveryTransport::DeliveryTransport(LogosModules& modules) : modules_(modules), peers_(default_store_peers("logos.dev")), cluster_(3) {}

std::vector<std::string> DeliveryTransport::default_store_peers(const std::string& preset) {
    if (preset == "logos.dev")  // the development fleet, cluster 3 (logos-delivery networks_config)
        return {
            "/dns4/delivery-01.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmTUbnxLGT9JvV6mu9oPyDjqHK4Phs1VDJNUgESgNSkuby",
            "/dns4/delivery-02.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmMK7PYygBtKUQ8EHp7EfaD3bCEsJrkFooK8RQ2PVpJprH",
            "/dns4/delivery-01.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm4S1JYkuzDKLKQvwgAhZKs9otxXqt8SCGtB4hoJP1S397",
            "/dns4/delivery-02.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8Y9kgBNtjxvCnf1X6gnZJW5EGE4UwwCL3CCm55TwqBiH",
            "/dns4/delivery-01.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8YokiNun9BkeA1ZRmhLbtNUvcwRr64F69tYj9fkGyuEP",
            "/dns4/delivery-02.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAkvwhGHKNry6LACrB8TmEFoCJKEX29XR5dDUzk3UT3UNSE",
        };
    return {
        "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmQ9X2xDfPG3uL77V9piYDhjq14JhKCtcmNYsTMKNqrKCj",
        "/dns4/node-01.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmF8WtwGPmeGHgYAX2277jHgy5cW9F7zsB8EqUjBZQAZQ3",
        "/dns4/node-01.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmL3oU95jh1BZHozn3uNhx8HEneirgr8M1jEAapzXGDqRF",
        "/dns4/node-02.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmB8NYprrfQrgWVzsJtYWkfjsXbmJEGNMG6othXsQ53BwG",
        // Added to the fleet with testnet v0.3 (logos-delivery v0.39.0 networks_config).
        "/dns4/node-02.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmUuXhUW9bdJpzN1kfDziFiUZo4bszTk66cvr7uuyCHXR7",
        "/dns4/node-02.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAm28CoBZjpyxsanC8tQpbvZ7bZJnVYuB1EgFzb571qpWsV",
    };
}

forum::SendResult DeliveryTransport::send(const std::string& content_topic, const std::string& payload) {
    std::weak_ptr<bool> alive = alive_;
    modules_.delivery_module.sendAsync(qs(content_topic), QByteArray::fromStdString(payload), [this, alive, payload](LogosResult r) {
        const auto a = alive.lock();
        if (!a || !*a) return;
        if (on_send_result) on_send_result(payload, r.success, string_of(r), error_of(r));
    });
    return {true, {}, {}, false};
}

bool DeliveryTransport::subscribe(const std::string& content_topic) {
    // Best effort from the engine; the backend subscribes with retries itself.
    modules_.delivery_module.subscribeAsync(qs(content_topic), [](LogosResult) {});
    return true;
}

std::vector<std::string> DeliveryTransport::history(const std::string&) { return {}; }

struct DeliveryTransport::Query {
    std::string topic;
    std::function<void(std::vector<std::string>)> done;
    size_t peer = 0;
    int page = 0;
    std::string cursor;
    bool answered = false;
    size_t from_peer = 0;
    std::vector<std::string> out;
};

void DeliveryTransport::history_async(const std::string& content_topic,
                                      std::function<void(std::vector<std::string>)> done) {
    last_peer_.clear();
    last_error_.clear();
    auto q = std::make_shared<Query>();
    q->topic = content_topic;
    q->done = std::move(done);
    query_page(q);
}

void DeliveryTransport::query_page(std::shared_ptr<Query> q) {
    if (q->peer >= peers_.size()) {
        if (last_peer_.empty() && last_error_.empty()) last_error_ = "no store node answered";
        q->done(std::move(q->out));
        return;
    }
    json req{{"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()},
             {"includeData", true},
             {"paginationForward", true},
             {"paginationLimit", kHistoryPageLimit},
             {"pubsubTopic", forum::pubsub_topic(q->topic, cluster_)},
             {"contentTopics", json::array({q->topic})}};
    // Diagnostics: ask for the whole shard, to tell "the node keeps nothing"
    // from "the node keeps nothing of ours".
    if (qEnvironmentVariableIsSet("LOGOS_FORUM_DIAG_SHARD")) req.erase("contentTopics");
    if (qEnvironmentVariable("LOGOS_FORUM_DIAG_SHARD") == QLatin1String("all")) req.erase("pubsubTopic");
    if (!q->cursor.empty()) req["paginationCursor"] = q->cursor;
    const std::string peer = peers_[q->peer];
    std::weak_ptr<bool> alive = alive_;
    modules_.delivery_module.storeQueryAsync(
        qs(req.dump()), qs(peer), kHistoryTimeoutMs,
        [this, alive, q, peer](LogosResult r) {
            const auto a = alive.lock();
            if (!a || !*a) return;
            // One peer is finished with — answered or not — when it fails, runs
            // out of pages, or hits the page bound.
            // Every store node is asked: retention differs between them, and
            // the engine de-duplicates by post id, so asking more costs only time.
            auto next_peer = [&]() {
                if (q->answered) {
                    if (!last_peer_.empty()) last_peer_ += ", ";
                    last_peer_ += peer.substr(0, peer.find("/tcp")) + " (" + std::to_string(q->from_peer) + ")";
                }
                ++q->peer;
                q->page = 0;
                q->cursor.clear();
                q->answered = false;
                q->from_peer = 0;
                query_page(q);
            };
            if (!r.success) {
                last_error_ = error_of(r);
                next_peer();
                return;
            }
            // The Qt wrapper may hand the response back as JSON text or as an
            // already-structured variant; normalise to text.
            QString text = r.value.toString();
            if (text.isEmpty()) text = QString::fromUtf8(QJsonDocument::fromVariant(r.value).toJson(QJsonDocument::Compact));
            const json resp = json::parse(text.toStdString(), nullptr, false);
            if (!resp.is_object() || !resp.contains("messages") || !resp["messages"].is_array()) {
                last_error_ = "unreadable store response";
                next_peer();
                return;
            }
            {
                const auto code = resp.find("statusCode");
                const auto desc = resp.find("statusDesc");
                last_status_ = (code != resp.end() ? code->dump() : std::string("?")) + " " +
                               (desc != resp.end() && desc->is_string() ? desc->get<std::string>() : std::string());
                // A store node that refuses the query (non-2xx) has not answered.
                if (code != resp.end() && code->is_number_integer() && (code->get<int>() < 200 || code->get<int>() >= 300)) {
                    last_error_ = "store node said " + last_status_;
                    next_peer();
                    return;
                }
            }
            q->answered = true;
            for (const auto& m : resp["messages"]) {
                if (!m.is_object() || !m.contains("message") || !m["message"].is_object()) continue;
                const auto& msg = m["message"];
                if (!msg.contains("payload") || !msg["payload"].is_string()) continue;
                std::string bytes = b64decode(msg["payload"].get<std::string>());
                if (!bytes.empty()) {
                    q->out.push_back(std::move(bytes));
                    ++q->from_peer;
                }
            }
            // A last page carries "paginationCursor": null, not an absent key.
            const auto c = resp.find("paginationCursor");
            q->cursor = c != resp.end() && c->is_string() ? c->get<std::string>() : std::string();
            if (q->cursor.empty() || resp["messages"].empty() || ++q->page >= kHistoryMaxPages) {
                next_peer();
                return;
            }
            query_page(q);
        },
        Timeout(kHistoryTimeoutMs + 7000));
}
