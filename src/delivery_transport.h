// SPDX-License-Identifier: MIT OR Apache-2.0
//
// forum::Transport over Logos Delivery.
//
// Sends and subscriptions forward to delivery_module one to one. History is a
// Store query against the network's store nodes: the first peer that answers
// is used, and its pages are followed up to a bound, so a forum that was busy
// while we were away is caught up in one call.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "forum/engine.h"

// The per-build aggregate behind LogosUiPluginContext::modules(). Declared
// here so this header stays free of the generated umbrella.
struct LogosModules;

class DeliveryTransport : public forum::Transport {
public:
    explicit DeliveryTransport(LogosModules& modules);

    // Asynchronous: delivery_module's send can wait up to 30 s on a stalled
    // node, and a blocking call would freeze the backend with it. send()
    // returns "accepted, not confirmed" at once; the module's answer comes to
    // `on_send_result` with the payload it was for.
    forum::SendResult send(const std::string& content_topic, const std::string& payload) override;
    bool subscribe(const std::string& content_topic) override;
    std::function<void(const std::string& payload, bool ok, const std::string& request_id, const std::string& error)>
        on_send_result;
    // Always empty: a Store query can take many seconds, and a blocking call
    // from the backend freezes the view with it. Use history_async().
    std::vector<std::string> history(const std::string& content_topic) override;

    // Ask the store nodes in turn, following pages, and call `done` with every
    // payload once one has answered (or all have failed).
    void history_async(const std::string& content_topic, std::function<void(std::vector<std::string>)> done);

    void set_store_peers(std::vector<std::string> peers) { peers_ = std::move(peers); }
    const std::vector<std::string>& store_peers() const { return peers_; }
    // Which peer answered the last history query, and how many messages it
    // returned; empty when none answered.
    const std::string& last_history_peer() const { return last_peer_; }
    const std::string& last_history_error() const { return last_error_; }
    // The store node's own verdict on the last page ("200 OK", …), for the log.
    const std::string& last_history_status() const { return last_status_; }

    // The logos.test fleet, which serves Store queries.
    static std::vector<std::string> default_store_peers();

private:
    struct Query;
    void query_page(std::shared_ptr<Query> q);
    LogosModules& modules_;
    std::vector<std::string> peers_;
    std::string last_peer_, last_error_, last_status_;
};
