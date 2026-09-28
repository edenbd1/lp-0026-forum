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

DeliveryTransport::DeliveryTransport(LogosModules& modules) : modules_(modules), peers_(default_store_peers()) {}

std::vector<std::string> DeliveryTransport::default_store_peers() {
    return {
        "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmQ9X2xDfPG3uL77V9piYDhjq14JhKCtcmNYsTMKNqrKCj",
        "/dns4/node-01.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmF8WtwGPmeGHgYAX2277jHgy5cW9F7zsB8EqUjBZQAZQ3",
        "/dns4/node-01.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmL3oU95jh1BZHozn3uNhx8HEneirgr8M1jEAapzXGDqRF",
        "/dns4/node-02.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmB8NYprrfQrgWVzsJtYWkfjsXbmJEGNMG6othXsQ53BwG",
    };
}

forum::SendResult DeliveryTransport::send(const std::string& content_topic, const std::string& payload) {
    const LogosResult r = modules_.delivery_module.send(qs(content_topic), QByteArray::fromStdString(payload));
    if (!r.success) return {false, error_of(r), {}};
    return {true, {}, string_of(r)};
}

bool DeliveryTransport::subscribe(const std::string& content_topic) {
    return modules_.delivery_module.subscribe(qs(content_topic)).success;
}

std::vector<std::string> DeliveryTransport::history(const std::string& content_topic) {
    std::vector<std::string> out;
    last_peer_.clear();
    last_error_.clear();
    for (const auto& peer : peers_) {
        std::string cursor;
        bool answered = false;
        for (int page = 0; page < kHistoryMaxPages; ++page) {
            json q{{"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()},
                   {"includeData", true},
                   {"paginationForward", true},
                   {"paginationLimit", kHistoryPageLimit},
                   {"contentTopics", json::array({content_topic})}};
            if (!cursor.empty()) q["paginationCursor"] = cursor;
            const LogosResult r = modules_.delivery_module.storeQuery(qs(q.dump()), qs(peer), kHistoryTimeoutMs);
            if (!r.success) {
                last_error_ = error_of(r);
                break;
            }
            // The Qt wrapper may hand the response back as JSON text or as an
            // already-structured variant; normalise to text.
            QString text = r.value.toString();
            if (text.isEmpty()) text = QString::fromUtf8(QJsonDocument::fromVariant(r.value).toJson(QJsonDocument::Compact));
            const json resp = json::parse(text.toStdString(), nullptr, false);
            if (!resp.is_object() || !resp.contains("messages") || !resp["messages"].is_array()) {
                last_error_ = "unreadable store response";
                break;
            }
            answered = true;
            for (const auto& m : resp["messages"]) {
                if (!m.is_object() || !m.contains("message") || !m["message"].is_object()) continue;
                const auto& msg = m["message"];
                if (!msg.contains("payload") || !msg["payload"].is_string()) continue;
                std::string bytes = b64decode(msg["payload"].get<std::string>());
                if (!bytes.empty()) out.push_back(std::move(bytes));
            }
            cursor = resp.value("paginationCursor", std::string());
            if (cursor.empty() || resp["messages"].empty()) break;
        }
        if (answered) {
            last_peer_ = peer;
            return out;  // one store node's view is the network's; no need to ask the rest
        }
    }
    return out;
}
