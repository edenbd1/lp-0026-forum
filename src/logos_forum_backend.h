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
#include <functional>
#include <optional>
#include <set>

#include <QSet>
#include <string>
#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
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
    QString markRead(QString topicId) override;

    QString createAccount(QString label) override;
    QString selectAccount(QString label) override;
    QString deleteAccount(QString label) override;
    QString rotateAccount() override;
    QString setRotation(int maxPosts, int maxDays) override;

    QString catchUp() override;
    QString saveSnapshot() override;
    QString useStorePeers(QString peers) override;
    QString retryMembership() override;
    QString openLink(QString url) override;

protected:
    void onContextReady() override;

private:
    void bootstrap();
    void wireDelivery();
    void wireStorage();
    void startNode();
    std::vector<std::string> dnsServers() const;  // name servers handed to Delivery, empty for its defaults
    void subscribe();
    void refreshStatus();
    void setRln(const QString& phase, const QString& detail);
    QString rlnShort() const;
    void publishRln();
    void pollMembership();
    void membershipAnswered(const QJsonObject& o);
    void membershipsAnswered(const QJsonObject& o);
    // A membership gifted by the forum's sponsor (rln_gifter_module).
    void requestGift();
    void giftLibp2p(const QString& method, const QString& arg, int attempt, std::function<void(QString)> done);
    void giftRegister();
    void giftFailed(const QString& reason);

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
    void failUpload(const QString& why);
    void fetchSnapshot(const forum::Announcement& an);
    void historyOverDelivery(const char* why);
    void learnStorageIdentity();

    QString dataDir() const;
    void loadSettings();
    void saveSettings();
    void saveSettingsSoon();
    void postArrivedSoon(const QString& id, const QString& topicId);
    void initStorage(bool withDiscPort);
    void armUploadWatchdog();
    QJsonObject readJson() const;

    std::unique_ptr<forum::Store> store_;
    std::unique_ptr<DeliveryTransport> net_;
    std::unique_ptr<forum::Engine> engine_;
    std::vector<forum::Account> accounts_;
    QString selectedLabel_;
    forum::RotationPolicy rotation_;
    QHash<QString, double> readUpTo_;  // topic id -> last activity the user has seen
    double readSince_ = 0;             // before this, everything counts as read (first launch)

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
    int storagePort_ = 0;
    bool uploading_ = false;
    QString uploadSession_;
    std::string lastSnapshotCid_;
    std::string answerRe_;          // the history request our next announcement answers
    uint64_t lastDeliveryAsk_ = 0;
    std::string uploadDoc_;
    size_t uploadOffset_ = 0, uploadPosts_ = 0, postsAtLastSnapshot_ = 0;
    QString downloadSession_;
    std::string downloadBuf_;
    std::set<std::string> seenSnapshots_;
    QString lastCatchUp_, lastSnapshot_;
    int uploadProgress_ = 0;        // upload watchdog generation, bumped at every upload step
    bool fetchSnapshots_ = false;
    QString preset_ = QStringLiteral("logos.test");  // the Logos Delivery network
    // RLN membership, on a network that runs RLN. rlnPhase_: "" (no RLN),
    // starting, wallet, gifting, gift-failed, funding, registering, pending,
    // active, quota, lapsed, failed, missing (the RLN modules are not loaded).
    QString networkSetting_;  // settings.json "network": the user's opt-in network, or empty
    QString rlnPhase_, rlnDetail_;
    QString rlnRegistry_, rlnIdentifier_;          // from delivery's rlnState
    QString rlnPayer_, rlnNeeds_, rlnPrice_, rlnHolds_;  // while awaiting funding
    int rlnRate_ = 0;
    bool rlnPolling_ = false;
    QTimer rlnTimer_;
    // The sponsor: an open gifter that registers a membership for this node
    // and pays for it. Empty peer id = sponsorship off ("gifter": "off").
    QString gifterSetting_;               // settings.json "gifter", kept across saves
    QString gifterPeer_, gifterAddr_;
    bool gifting_ = false;                // a request is on its way
    bool libp2pUp_ = false;
    int giftFailures_ = 0;
    qint64 giftRetryAt_ = 0;              // ms since epoch; 0 = ask now
    QString giftError_;
    QSet<QString> quotaHeld_;  // our posts delivery holds for the epoch's quota
    QJsonValue dnsSetting_;         // settings.json "dns": name servers chosen by hand
    QSet<QString> earlyConfirms_;   // confirmations that arrived before our send reply
    QList<QString> earlyOrder_;     // their arrival order, to drop the oldest
    std::optional<forum::Announcement> pendingAnnouncement_;  // answered before storage was ready
    std::function<void()> manifestNext_;  // the chunk download to start once the manifest is found
    QString manifestCid_;

    // Arrivals are told to the view at most once per short window: a large
    // import would otherwise mean one full refresh per post.
    QHash<QString, QString> arrivals_;  // topic id -> latest post id
    bool arrivalsScheduled_ = false;
    bool settingsScheduled_ = false;

    // Local activity counters, logged once an hour to forum.log and nowhere
    // else. Counts only: no request id, key or address is kept.
    void countActivity(const std::string& payload, bool newPost);
    void logActivity();
    int actRequests_ = 0, actFresh_ = 0, actPosts_ = 0;
    QTimer activityTimer_;
};
