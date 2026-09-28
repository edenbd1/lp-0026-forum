// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The Basecamp side of the forum: it owns the core (store, engine, accounts)
// and connects it to Logos Delivery for posts and Logos Storage for history
// snapshots. Every rule — what is accepted, stored, sent, retried and paced —
// lives in core/ and is tested there; this class only moves bytes and events
// between the core, the two modules and the QML view.
//
// Keys are Ed25519 and kept in this install's store, not in a shared signer:
// every reader has to be able to check every signature, and anonymous posts
// need a key that exists for one post and is then wiped.
#pragma once

#include <memory>
#include <set>
#include <string>

#include <QHash>
#include <QString>
#include <QTimer>

#include "delivery_transport.h"
#include "forum/engine.h"
#include "forum/identity.h"
#include "forum/store.h"
#include "logos_ui_plugin_context.h"
#include "rep_logos_forum_source.h"

class LogosForumBackend : public LogosForumSimpleSource, public LogosUiPluginContext {
public:
    LogosForumBackend();
    ~LogosForumBackend() override;

    QString createTopic(QString title, QString body, int mode, QString alias) override;
    QString reply(QString topicId, QString body, int mode, QString alias) override;
    QString listTopics() override;
    QString thread(QString topicId) override;

    QString createAccount(QString label) override;
    QString selectAccount(QString label) override;
    QString deleteAccount(QString label) override;
    QString rotateAccount() override;
    QString setRotation(int maxPosts, int maxDays) override;

    QString catchUp() override;
    QString saveSnapshot() override;
    QString useStorePeers(QString peers) override;

protected:
    void onContextReady() override;

private:
    void bootstrap();
    void wireDelivery();
    void wireStorage();
    void startNode();
    void subscribe();
    void refreshStatus();

    // Posting
    QString compose(bool topic, const QString& target, const QString& text, const QString& title, int mode,
                    const QString& alias);
    forum::Account* selected();
    void publishAccounts();
    void publishOutbox();
    void pump();

    // History
    void runCatchUp(const char* why);
    void publishHistory();
    void uploadNextChunk();
    void finishUpload();
    void fetchSnapshot(const std::string& cid);

    QString dataDir() const;
    void loadSettings();
    void saveSettings();

    std::unique_ptr<forum::Store> store_;
    std::unique_ptr<DeliveryTransport> net_;
    std::unique_ptr<forum::Engine> engine_;
    std::vector<forum::Account> accounts_;
    QString selectedLabel_;
    forum::RotationPolicy rotation_;

    QTimer pumpTimer_, historyTimer_, snapshotTimer_;
    QString connectionState_;
    bool subscribed_ = false;
    bool catchingUp_ = false;
    int subscribeAttempts_ = 0;

    // delivery request id -> post id, for posts the network has accepted but
    // not yet settled.
    QHash<QString, QString> inFlight_;

    // Storage
    bool storageReady_ = false;
    QString uploadSession_;
    std::string uploadDoc_;
    size_t uploadOffset_ = 0, uploadPosts_ = 0, postsAtLastSnapshot_ = 0;
    QString downloadSession_;
    std::string downloadBuf_;
    std::set<std::string> seenSnapshots_;
    QString lastCatchUp_, lastSnapshot_;
};
