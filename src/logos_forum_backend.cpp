// SPDX-License-Identifier: MIT OR Apache-2.0
#include "logos_forum_backend.h"

#include <algorithm>
#include <iostream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHostAddress>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVariant>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include "logos_sdk.h"
#include "logos_types.h"

using forum::Account;
using forum::Kind;
using forum::Mode;
using forum::Post;

namespace {

// Basecamp does not surface a ui-host's stderr, so the log also goes to
// forum.log in the forum's data directory, where tests and bug reports read it.
QString g_logPath;
void log(const std::string& m) {
    const std::string line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString() + " " + m;
    std::cerr << "[logos_forum] " << line << std::endl;
    if (g_logPath.isEmpty()) return;
    // Kept bounded: one previous file, 5 MB each.
    if (QFileInfo(g_logPath).size() > 5 * 1024 * 1024) {
        QFile::remove(g_logPath + QStringLiteral(".1"));
        QFile::rename(g_logPath, g_logPath + QStringLiteral(".1"));
    }
    QFile f(g_logPath);
    if (f.open(QIODevice::Append | QIODevice::Text)) f.write((line + "\n").c_str());
}

uint64_t now_ms() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s(const QString& v) { return v.toStdString(); }

QString hhmm(uint64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms)).toString(QStringLiteral("HH:mm"));
}

// A module call may come back as a LogosResult or a plain bool depending on
// the method's contract; both mean the same thing here.
bool ok(const LogosResult& r) { return r.success; }
bool ok(bool b) { return b; }

std::string b64decode(const std::string& in) {
    std::string out(in.size(), '\0');
    size_t n = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), in.data(), in.size(), " \n",
                          &n, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
        return {};
    out.resize(n);
    return out;
}

// Module events are parsed without ever throwing: an exception escaping an
// event callback aborts the ui-host process.
std::string str_at(const nlohmann::json& j, const char* k) {
    if (!j.is_object()) return {};
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool bool_at(const nlohmann::json& j, const char* k) {
    if (!j.is_object()) return false;
    const auto it = j.find(k);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

constexpr const char* kDefaultForum = "Logos Forum";
// The forum's membership sponsor: an open RLN gifter (logos-rln-gifter,
// LIP-158) run by the forum's author, which registers a membership for any
// node that asks and pays for it on the registry's zone. gifter/ in this repo.
constexpr const char* kGifter = "/ip4/88.160.11.28/tcp/24026/p2p/16Uiu2HAm4XsEj65CPBnXZTZbniEE9SEiRtJUmngGuQxQoGFSHxA6";
// The registry's minimum, and what the sponsor grants: messages per 10-minute epoch.
constexpr const char* kGiftRate = "100";
constexpr int kPumpMs = 2000;
constexpr int kCatchUpMs = 5 * 60 * 1000;
constexpr int kSnapshotMs = 30 * 60 * 1000;
constexpr int kMaxSubscribeAttempts = 10;
constexpr int kSubscribeRetryMs = 3000;
constexpr size_t kUploadChunk = 48 * 1024;
constexpr int kDownloadChunk = 64 * 1024;
constexpr size_t kMaxSnapshotBytes = 32u * 1024 * 1024;
constexpr int kDownloadWatchdogMs = 120000;  // a DHT lookup over Mix can be slow
constexpr int kUploadWatchdogMs = 45000;
constexpr int kArrivalBatchMs = 150;

// Same-machine test rigs run every node on 127.0.0.1 or one LAN: only there
// may announcements name, and fetches dial, private addresses.
bool localPeersAllowed() { return qEnvironmentVariableIsSet("LOGOS_FORUM_LOCAL_PEERS"); }

// Fetching a snapshot from Logos Storage means connecting to whoever provides
// it, which tells them this node's address. History comes over Delivery relays
// instead, unless the user opts in (LOGOS_FORUM_FETCH_SNAPSHOTS, or
// "fetchSnapshots": true in settings.json).
bool g_fetchSnapshots = false;
// Snapshots travel over Mix unless LOGOS_FORUM_SNAPSHOTS_DIRECT is set: the
// storage node joins the network's Mix (mix-enabled) and downloads tunnel
// through it, so fetching one no longer tells the provider our address.
bool g_snapshotsOverMix = true;

// Anything thrown inside a module event or timer callback would abort the
// ui-host: log it and drop the message instead.
template <typename F> void guarded(const char* what, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        log(std::string(what) + ": dropped after an error: " + e.what());
    }
}

} // namespace

LogosForumBackend::LogosForumBackend() {
    setAppVersion(QStringLiteral(LOGOS_FORUM_VERSION));
    const QString name = qEnvironmentVariable("LOGOS_FORUM_NAME", QString::fromLatin1(kDefaultForum));
    setForumName(name);
    setContentTopic(q(forum::content_topic(s(name))));
}

LogosForumBackend::~LogosForumBackend() {
    if (settingsScheduled_) saveSettings();  // a read marker saved "soon" is not lost on quit
}

QString LogosForumBackend::dataDir() const {
    // Basecamp's --user-dir is exported to every child process as
    // LOGOS_USER_DIR; keep the forum inside it so two Basecamp instances on one
    // machine are two independent users.
    const QString userDir = qEnvironmentVariable("LOGOS_USER_DIR");
    const QString dir = userDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logos_forum")
        : userDir + QStringLiteral("/module_data/logos_forum");
    QDir().mkpath(dir);
    return dir;
}

void LogosForumBackend::onContextReady() {
    // Node creation blocks briefly; return first so the view's replica can
    // come up, then bootstrap on the next turn of the event loop.
    QTimer::singleShot(0, this, [this]() { bootstrap(); });
}

void LogosForumBackend::bootstrap() {
    g_logPath = dataDir() + QStringLiteral("/forum.log");
    log("bootstrap: forum \"" + s(forumName()) + "\" on " + s(contentTopic()));
    if (sodium_init() < 0) {
        setStatus(QStringLiteral("libsodium failed to initialise"));
        return;
    }
    try {
        store_ = std::make_unique<forum::Store>(s(dataDir() + QStringLiteral("/forum.db")));
    } catch (const std::exception& e) {
        setStatus(QStringLiteral("Local store unavailable: %1").arg(e.what()));
        return;
    }
    net_ = std::make_unique<DeliveryTransport>(modules());
    // Which Logos Delivery network. logos.test, the testnet v0.3 network, by
    // default: there every sender needs an RLN membership, which the forum's
    // sponsor registers and pays for (requestGift below). logos.dev, which
    // runs no RLN, stays selectable ("network": "logos.dev" in settings.json,
    // or LOGOS_FORUM_PRESET, which wins).
    QJsonObject settings;
    {
        QFile f(dataDir() + QStringLiteral("/settings.json"));
        if (f.open(QIODevice::ReadOnly)) settings = QJsonDocument::fromJson(f.readAll()).object();
    }
    networkSetting_ = settings.value(QLatin1String("network")).toString();
    preset_ = qEnvironmentVariable("LOGOS_FORUM_PRESET", networkSetting_);
    if (preset_ != QLatin1String("logos.dev") && preset_ != QLatin1String("logos.test")) {
        if (!preset_.isEmpty()) log("unknown network \"" + s(preset_) + "\": using logos.test");
        preset_ = QStringLiteral("logos.test");
    }
    // The sponsor: "<multiaddr>/p2p/<peer id>" in settings.json "gifter" or
    // LOGOS_FORUM_GIFTER (which wins), "off" to pay for a membership yourself.
    gifterSetting_ = settings.value(QLatin1String("gifter")).toString();
    {
        const QString g = qEnvironmentVariable("LOGOS_FORUM_GIFTER", gifterSetting_).trimmed();
        const QString spec = g.isEmpty() ? QString::fromLatin1(kGifter) : g;
        const int at = spec.indexOf(QLatin1String("/p2p/"));
        if (spec != QLatin1String("off") && at > 0) {
            gifterAddr_ = spec.left(at);
            gifterPeer_ = spec.mid(at + 5);
        }
        log("membership sponsor: " + (gifterPeer_.isEmpty() ? std::string("off") : s(gifterAddr_) + "/p2p/" + s(gifterPeer_)));
    }
    net_->set_cluster(DeliveryTransport::cluster_of(s(preset_)));
    net_->set_store_peers(DeliveryTransport::default_store_peers(s(preset_)));
    log("network preset " + s(preset_) + ", cluster " + std::to_string(DeliveryTransport::cluster_of(s(preset_))));
    loadSettings();

    // The engine subscribes in its constructor; that first attempt may fail
    // while the node is not up yet, and subscribe() below retries.
    engine_ = std::make_unique<forum::Engine>(*store_, *net_, s(forumName()));
    engine_->on_post = [this](const Post& p, const std::string& id) {
        postArrivedSoon(q(id), q(p.kind == Kind::Topic ? id : p.topic_id));
    };
    net_->on_send_result = [this](const std::string& payload, bool sent, const std::string& request,
                                  const std::string& error) {
        guarded("send result", [&]() {
            const auto p = forum::decode(payload);
            if (!p || !engine_) return;  // a history answer or a request: nothing to track
            const std::string id = p->id();
            if (sent) {
                log("handed " + id.substr(0, 12) + " to delivery as request " + request);
                if (request.empty()) return;
                // The network may have confirmed it before this reply reached us.
                if (earlyConfirms_.remove(q(request)) > 0) {
                    engine_->confirm(id);
                    log("confirmed " + id.substr(0, 12) + " (before its send reply)");
                    publishOutbox();
                    emit postStateChanged(q(id), QStringLiteral("sent"), QString());
                    return;
                }
                inFlight_.insert(q(request), q(id));
            } else {
                log("delivery refused " + id.substr(0, 12) + ": " + error);
                if (engine_->requeue(id, error.empty() ? "send failed" : error, now_ms())) publishOutbox();
                emit postStateChanged(q(id), QStringLiteral("failed"), q(error));
            }
        });
    };
    // Accepted by the node, not yet out: a post stays in the outbox until the
    // network confirms it (messagePropagated / messageSent below). The request
    // id arrives with the asynchronous send result (on_send_result above).
    g_fetchSnapshots = qEnvironmentVariableIsSet("LOGOS_FORUM_FETCH_SNAPSHOTS") || fetchSnapshots_;
    g_snapshotsOverMix = !qEnvironmentVariableIsSet("LOGOS_FORUM_SNAPSHOTS_DIRECT");
    if (g_fetchSnapshots) engine_->on_snapshot = [this](const forum::Announcement& an) { fetchSnapshot(an); };
    log(std::string("history from peers: ") + (g_fetchSnapshots ? "Logos Storage snapshots, then Delivery" : "Delivery only"));
    // Storage-path answers announce this node's storage address: only when the
    // user opted in. Otherwise the engine answers every request over Delivery.
    if (g_fetchSnapshots) engine_->on_history_wanted = [this](const forum::HistoryRequest& r) {
        log("answering a history request from a peer with " + std::to_string(r.have) + " posts");
        answerRe_ = r.id;
        // Off the delivery event's call stack: answering calls back into modules.
        QTimer::singleShot(0, this, [this]() {
            if (!lastSnapshotCid_.empty() && postsAtLastSnapshot_ == store_->count())
                engine_->announce_snapshot(lastSnapshotCid_, postsAtLastSnapshot_, now_ms(), answerRe_);  // unchanged: same CID
            else
                saveSnapshot();
        });
    };

    // Accounts: the first run makes one, so posting works straight away.
    accounts_ = store_->accounts();
    if (const auto sel = store_->selected_label()) selectedLabel_ = q(*sel);
    if (accounts_.empty()) createAccount(QString());
    else if (!selected()) selectedLabel_ = q(accounts_.front().label);
    publishAccounts();
    publishOutbox();
    postsAtLastSnapshot_ = store_->count();
    setNodeReady(true);  // composing needs only the local store

    wireDelivery();
    wireStorage();
    startNode();

    connect(&activityTimer_, &QTimer::timeout, this, [this]() { logActivity(); });
    activityTimer_.start(qEnvironmentVariableIntValue("LOGOS_FORUM_ACTIVITY_MS") > 0
                             ? qEnvironmentVariableIntValue("LOGOS_FORUM_ACTIVITY_MS") : 3600000);  // override for tests
    rlnTimer_.setSingleShot(true);
    connect(&rlnTimer_, &QTimer::timeout, this, [this]() { pollMembership(); });
    connect(&pumpTimer_, &QTimer::timeout, [this]() { pump(); });
    pumpTimer_.start(kPumpMs);
    connect(&historyTimer_, &QTimer::timeout, [this]() { runCatchUp("periodic"); });
    historyTimer_.start(kCatchUpMs);
    if (g_fetchSnapshots) connect(&snapshotTimer_, &QTimer::timeout, [this]() {
        if (store_ && store_->count() != postsAtLastSnapshot_) saveSnapshot();
    });
    snapshotTimer_.start(kSnapshotMs);
}

// ─── Logos Delivery ─────────────────────────────────────────────────────────

void LogosForumBackend::wireDelivery() {
    auto& d = modules().delivery_module;
    d.on("connectionStateChanged", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QString prev = connectionState_;
        connectionState_ = data.at(0).toString();
        log("connection: " + s(connectionState_));
        refreshStatus();
        // Back from offline: fetch what was missed and flush the outbox.
        if (connectionState_ == QLatin1String("Connected") && prev != connectionState_)
            QTimer::singleShot(0, this, [this]() {
                runCatchUp("reconnected");
                if (engine_ && engine_->reconnected(now_ms()) > 0) publishOutbox();
            });
    });
    d.on("messageReceived", [this](const QVariantList& data) {
        if (data.size() < 3 || !engine_) return;
        if (data.at(1).toString() != contentTopic()) return;  // another app's traffic
        guarded("received message", [&]() {
            const std::string payload = data.at(2).toByteArray().toStdString();
            const size_t before = store_->count();
            engine_->receive(payload, now_ms());
            const size_t added = store_->count() - before;
            countActivity(payload, added > 0 && payload.find("\"logos-forum-snapshot\"") == std::string::npos);
            if (payload.find("\"logos-forum-snapshot\"") != std::string::npos) {
                log("received a history bundle: " + std::to_string(added) + " new");
                if (added > 0) {
                    lastSnapshot_ = QStringLiteral("History from peers · %1 new").arg(added);
                    publishHistory();
                }
            } else {
                log(std::string("received ") + (added ? "a new post" : "a known or foreign message"));
            }
        });
    });
    d.on("nodeStarted", [this](const QVariantList& data) {
        const bool up = !data.isEmpty() && data.at(0).toBool();
        log(std::string("nodeStarted ") + (up ? "ok" : "failed"));
        if (!up) {
            const QString why = data.value(1).toString();
            // An RLN network refuses a node without a membership: say what to do.
            setStatus(why.contains(QLatin1String("membership"), Qt::CaseInsensitive)
                ? QStringLiteral("This network needs an RLN membership: see the note below")
                : QStringLiteral("Node failed to start: %1").arg(why));
            return;
        }
        subscribeAttempts_ = 0;
        QTimer::singleShot(0, this, [this]() {
            subscribe();
            runCatchUp("node started");
        });
    });
    // Either event means the post reached the network: it leaves the outbox.
    auto confirmed = [this](const QVariantList& data, const char* how) {
        const QString request = data.value(0).toString();
        const auto it = inFlight_.constFind(request);
        if (it == inFlight_.constEnd()) {
            // Not ours, already confirmed, or ahead of our own send reply: keep
            // it briefly so that reply can still match it.
            // delivery_module is shared: other apps' confirmations land here too,
            // so the oldest are dropped rather than refusing ours.
            if (!request.isEmpty()) {
                earlyConfirms_.insert(request);
                earlyOrder_.append(request);
                while (earlyOrder_.size() > 512) earlyConfirms_.remove(earlyOrder_.takeFirst());
                QTimer::singleShot(30000, this, [this, request]() { earlyConfirms_.remove(request); });
            }
            return;
        }
        const QString id = it.value();
        inFlight_.remove(request);
        if (quotaHeld_.remove(id) && quotaHeld_.isEmpty() && rlnPhase_ == QLatin1String("quota"))
            setRln(QStringLiteral("active"), QString());
        if (engine_) engine_->confirm(s(id));
        log("confirmed " + s(id).substr(0, 12) + " (" + how + ")");
        publishOutbox();
        emit postStateChanged(id, QStringLiteral("sent"), QString());
    };
    d.on("messagePropagated", [confirmed](const QVariantList& data) { confirmed(data, "propagated"); });
    d.on("messageSent", [confirmed](const QVariantList& data) { confirmed(data, "sent"); });
    // RLN (rate-limiting nullifiers) comes with the network preset: on
    // logos.test (opt-in) every sender needs an active RLN membership, and
    // each 600 s epoch allows only so many messages. Delivery loads the RLN
    // modules when they are installed (they are optional, not the forum's
    // dependencies, so logos.dev users never download them);
    // liblogos_rln_module then registers a membership by itself once its LEZ
    // account holds the fee. The forum only watches: it says where the
    // membership stands and what a user has to do, and reading never waits.
    d.on("rlnStateChanged", [this](const QVariantList& data) {
        const QString state = data.value(0).toString(), message = data.value(1).toString();
        log("rln: " + s(state) + (message.isEmpty() ? "" : " (" + s(message) + ")"));
        if (state == QLatin1String("Disabled")) {
            setRln(QString(), QString());
        } else if (state == QLatin1String("Initializing")) {
            if (rlnPhase_.isEmpty()) setRln(QStringLiteral("starting"), QString());
        } else if (state == QLatin1String("Ready")) {
            if (rlnPhase_.isEmpty() || rlnPhase_ == QLatin1String("starting") || rlnPhase_ == QLatin1String("missing"))
                setRln(QStringLiteral("starting"), QString());
            pollMembership();
        } else if (state == QLatin1String("Failed")) {
            // The bridge could not reach liblogos_rln_module: it is not
            // installed, or Basecamp did not load it.
            const bool absent = message.contains(QLatin1String("object_unavailable"))
                || message.contains(QLatin1String("liblogos_rln_module"))
                || message.contains(QLatin1String("bridge unavailable"));
            setRln(absent ? QStringLiteral("missing") : QStringLiteral("failed"), message);
        }
    });
    d.on("messageQueued", [this](const QVariantList& data) {
        const QString request = data.value(0).toString();
        const auto it = inFlight_.constFind(request);
        if (it == inFlight_.constEnd()) return;  // not ours
        log("held by the RLN rate limit: " + s(it.value()).substr(0, 12) + " (goes out when the epoch's quota refills)");
        quotaHeld_.insert(it.value());
        if (rlnPhase_ == QLatin1String("active")) setRln(QStringLiteral("quota"), QString());
        emit postStateChanged(it.value(), QStringLiteral("queued"), QStringLiteral("rate limit"));
    });
    d.on("messageError", [this](const QVariantList& data) {
        const QString request = data.value(0).toString();
        const auto it = inFlight_.constFind(request);
        if (it == inFlight_.constEnd()) return;  // not ours: delivery_module is shared
        const QString id = it.value();
        const QString why = data.value(2).toString();
        inFlight_.remove(request);
        log("lost " + s(id).substr(0, 12) + ": " + s(why));
        // Accepted locally, then lost: back in the outbox, retried with back-off.
        if (engine_ && engine_->requeue(s(id), s(why), now_ms())) publishOutbox();
        emit postStateChanged(id, QStringLiteral("failed"), why);
    });
}

void LogosForumBackend::startNode() {
    // The layered config shape; bare node keys at top level would switch the
    // parser to the legacy shape and fixed ports (see forum-sample-app).
    const QString cfg = QString::fromUtf8(QJsonDocument(QJsonObject{{"mode", "Core"}, {"preset", preset_}}).toJson(QJsonDocument::Compact));
    setStatus(QStringLiteral("Starting node…"));
    // Asynchronous, like every delivery call: a stalled node must not freeze the backend.
    modules().delivery_module.createNodeAsync(cfg, [this](LogosResult created) {
        if (!created.success) {
            // delivery_module is shared across Basecamp apps: another may already
            // run the node, in which case no nodeStarted will come for us.
            log("createNode refused (node already running?): " + s(created.getError()));
            subscribe();
            runCatchUp("joined running node");
            return;
        }
        subscribe();  // before start(), so nothing that arrives early is missed
        modules().delivery_module.startAsync([this](LogosResult started) {
            if (!started.success) setStatus(QStringLiteral("Node failed to start: %1").arg(started.getError()));
        });
    });
}

void LogosForumBackend::subscribe() {
    if (subscribed_ || !net_) return;
    ++subscribeAttempts_;
    modules().delivery_module.subscribeAsync(contentTopic(), [this](LogosResult r) {
        if (subscribed_) return;
        if (r.success) {
            log("subscribed on attempt " + std::to_string(subscribeAttempts_));
            subscribed_ = true;
            refreshStatus();
            return;
        }
        // Usually the node is still bootstrapping; that passes. Posting is not
        // affected either way: it runs off the local store.
        setStatus(QStringLiteral("Joining the forum topic… (attempt %1)").arg(subscribeAttempts_));
        if (subscribeAttempts_ < kMaxSubscribeAttempts)
            QTimer::singleShot(kSubscribeRetryMs, this, [this]() { subscribe(); });
    });
}

void LogosForumBackend::refreshStatus() {
    if (!subscribed_) return;
    // A node with no peers still accepts sends; say so rather than "Connected".
    const QString base = connectionState_.isEmpty() ? QStringLiteral("Joined, waiting for peers") : connectionState_;
    const QString rln = rlnShort();
    const QString net = preset_ == QLatin1String("logos.dev") ? QString() : QStringLiteral(" · ") + preset_;
    setStatus(base + net + (rln.isEmpty() ? QString() : QStringLiteral(" · ") + rln));
}


// ─── RLN membership ─────────────────────────────────────────────────────────

namespace {

// Registry ids as delivery names them, and where each one's LEZ zone is: the
// account that pays for a membership has to be funded on that zone.
QString zoneOf(const QString& registry) {
    if (registry.endsWith(QLatin1String("841312e989c77e3f6f58a5d880a8e25b950b8b5ffba2f39748fa44622c20c893")))
        return QStringLiteral("the LEZ testnet zone of the RLN registry (sequencer http://209.38.241.182:3240)");
    return QStringLiteral("the LEZ zone of registry %1").arg(registry);
}

// LEZ wallets name public accounts in base58 ("Public/<base58>"); the RLN
// module reports its payer in hex.
QString base58(const QString& hex) {
    static const char* A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.size() != 32) return hex;
    std::vector<unsigned char> digits;  // base 58, least significant first
    for (unsigned char b : bytes) {
        int carry = b;
        for (auto& d : digits) {
            carry += d * 256;
            d = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        while (carry) {
            digits.push_back(static_cast<unsigned char>(carry % 58));
            carry /= 58;
        }
    }
    QString out;
    for (unsigned char b : bytes) {
        if (b) break;
        out += QLatin1Char('1');
    }
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) out += QLatin1Char(A[*it]);
    return out;
}

// A module reply may arrive as a map, a JSON string, or a JSON string inside
// a string; all mean the same object.
QJsonObject replyObject(const QVariant& v) {
    if (v.typeId() == QMetaType::QVariantMap) return QJsonObject::fromVariantMap(v.toMap());
    QByteArray raw = v.typeId() == QMetaType::QByteArray ? v.toByteArray() : v.toString().toUtf8();
    for (int i = 0; i < 2; ++i) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            const QJsonObject o = doc.object();
            // A LogosResult that crossed as a map.
            if (o.contains(QLatin1String("success")) && o.contains(QLatin1String("value"))) {
                const QJsonValue inner = o.value(QLatin1String("value"));
                if (inner.isObject()) return inner.toObject();
                raw = inner.toString().toUtf8();
                continue;
            }
            return o;
        }
        if (raw.startsWith('"')) {
            raw = QJsonDocument::fromJson("[" + raw + "]").array().at(0).toString().toUtf8();
            continue;
        }
        break;
    }
    return {};
}

QString grouped(const QString& digits) {
    QString out;
    for (int i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += QLatin1Char(',');
        out += digits.at(i);
    }
    return out;
}

} // namespace

void LogosForumBackend::setRln(const QString& phase, const QString& detail) {
    if (phase == rlnPhase_ && detail == rlnDetail_) return;
    if (phase != rlnPhase_) log("rln membership: " + s(phase.isEmpty() ? QStringLiteral("not used on this network") : phase)
                                + (detail.isEmpty() ? "" : " (" + s(detail) + ")"));
    rlnPhase_ = phase;
    rlnDetail_ = detail;
    publishRln();
    refreshStatus();
}

QString LogosForumBackend::rlnShort() const {
    if (rlnPhase_.isEmpty()) return {};
    if (rlnPhase_ == QLatin1String("active")) return QStringLiteral("RLN membership active");
    if (rlnPhase_ == QLatin1String("quota")) return QStringLiteral("RLN rate limit reached, posts held");
    if (rlnPhase_ == QLatin1String("gifting")) return QStringLiteral("getting an RLN membership from the sponsor");
    if (rlnPhase_ == QLatin1String("gift-failed")) return QStringLiteral("no RLN membership yet: posting waits");
    if (rlnPhase_ == QLatin1String("funding")) return QStringLiteral("no RLN membership yet: posting waits");
    if (rlnPhase_ == QLatin1String("registering") || rlnPhase_ == QLatin1String("pending"))
        return QStringLiteral("RLN membership registering");
    if (rlnPhase_ == QLatin1String("missing")) return QStringLiteral("RLN modules not installed: posting waits");
    if (rlnPhase_ == QLatin1String("failed") || rlnPhase_ == QLatin1String("lapsed"))
        return QStringLiteral("RLN membership unavailable: posting waits");
    return QStringLiteral("RLN starting…");
}

void LogosForumBackend::publishRln() {
    QString title, what;
    const QString payer = base58(rlnPayer_);
    const bool sponsored = !gifterPeer_.isEmpty();
    if (rlnPhase_ == QLatin1String("active")) {
        title = QStringLiteral("RLN membership active");
        what = QStringLiteral("Posts carry a rate-limit proof. %1 messages per 10-minute epoch.")
                   .arg(rlnRate_ > 0 ? QString::number(rlnRate_) : QStringLiteral("A fixed number of"));
    } else if (rlnPhase_ == QLatin1String("quota")) {
        title = QStringLiteral("Rate limit reached for this epoch");
        what = QStringLiteral("This node has used its RLN messages for the current 10-minute epoch. "
                              "Held posts go out by themselves when the next epoch starts.");
    } else if (rlnPhase_ == QLatin1String("gifting")) {
        title = QStringLiteral("Getting you a membership from the forum's sponsor…");
        what = QStringLiteral("logos.test accepts a post only with a rate-limit proof, which needs an RLN "
                              "membership. The forum's sponsor registers one for this node and pays for it: "
                              "there is nothing to do and nothing to pay. It takes a few minutes. Reading works "
                              "meanwhile; your posts wait in the outbox and go out once it is active.");
    } else if (rlnPhase_ == QLatin1String("gift-failed")) {
        title = QStringLiteral("The sponsor could not give a membership yet");
        const qint64 wait = giftRetryAt_ > 0 ? (giftRetryAt_ - static_cast<qint64>(now_ms())) / 60000 + 1 : 0;
        what = QStringLiteral("%1. Trying again %2. Reading works; your posts wait in the outbox.")
                   .arg(giftError_.isEmpty() ? QStringLiteral("No answer") : giftError_,
                        wait > 1 ? QStringLiteral("in %1 minutes").arg(wait) : QStringLiteral("in a minute"));
    } else if (rlnPhase_ == QLatin1String("funding")) {
        title = QStringLiteral("Posting needs an RLN membership");
        what = QStringLiteral("This network accepts a post only with a rate-limit proof, which needs an RLN "
                              "membership. The forum's sponsor is off (\"gifter\": \"off\" in settings.json), so "
                              "this node registers one by itself once the account below holds %1 "
                              "native units on %2 (it holds %3). Fund it with a transfer on that zone or a "
                              "deposit from the Logos blockchain; the price is %4 units, the rest is a fee reserve "
                              "and comes back unspent. Reading and history work meanwhile; your posts wait in "
                              "the outbox and go out once the membership is active.")
                   .arg(grouped(rlnNeeds_), zoneOf(rlnRegistry_), grouped(rlnHolds_.isEmpty() ? QStringLiteral("0") : rlnHolds_),
                        grouped(rlnPrice_.isEmpty() ? QStringLiteral("?") : rlnPrice_));
    } else if (rlnPhase_ == QLatin1String("registering") || rlnPhase_ == QLatin1String("pending")) {
        title = QStringLiteral("Registering the RLN membership");
        what = sponsored ? QStringLiteral("The forum's sponsor has sent your membership to the chain; it confirms in a "
                                          "few minutes. Your posts wait in the outbox until then.")
                         : QStringLiteral("The registration is on its way to the chain; it confirms in a few minutes. "
                                          "Your posts wait in the outbox until then.");
    } else if (rlnPhase_ == QLatin1String("wallet") || rlnPhase_ == QLatin1String("starting")) {
        title = QStringLiteral("Setting up the RLN membership");
        what = QStringLiteral("The RLN module is opening its LEZ wallet and reading the registry. "
                              "Reading works meanwhile.");
    } else if (rlnPhase_ == QLatin1String("missing")) {
        title = QStringLiteral("RLN modules missing");
        what = QStringLiteral("logos.test needs the RLN modules, which Basecamp installs with the forum. Reinstall "
                              "Logos Forum from its catalog (it brings liblogos_rln_module, liblogos_lez_rln_module, "
                              "libp2p_module and rln_gifter_module), then restart Basecamp. %1 Reading works "
                              "meanwhile; posts wait in the outbox.").arg(rlnDetail_);
    } else if (rlnPhase_ == QLatin1String("lapsed")) {
        title = QStringLiteral("RLN membership no longer usable");
        what = QStringLiteral("The membership is %1. %2 Posts wait in the outbox.")
                   .arg(rlnDetail_, sponsored ? QStringLiteral("The forum asks its sponsor for a new one.")
                                              : QStringLiteral("Restart Basecamp and the node registers a new one once its account is funded."));
    } else if (rlnPhase_ == QLatin1String("failed")) {
        title = QStringLiteral("RLN membership unavailable");
        what = QStringLiteral("%1. Reading works; posts wait in the outbox.").arg(rlnDetail_);
    }
    setRlnJson(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"on", !rlnPhase_.isEmpty()},
        {"phase", rlnPhase_},
        {"title", title},
        {"detail", what},
        {"payer", rlnPhase_ == QLatin1String("funding") ? payer : QString()},
        {"needs", rlnNeeds_},
        {"holds", rlnHolds_},
        {"retry", rlnPhase_ == QLatin1String("gift-failed")},
    }).toJson(QJsonDocument::Compact)));
}

namespace {
// liblogos_rln_module's reply, through a module client: the object, or an
// {"error":{class,kind,message}} envelope.
QString rlnError(const QJsonObject& o) {
    const QJsonObject err = o.value(QLatin1String("error")).toObject();
    if (err.isEmpty()) return o.value(QLatin1String("error")).toString();
    return err.value(QLatin1String("message")).toString(err.value(QLatin1String("kind")).toString(QStringLiteral("error")));
}
} // namespace

void LogosForumBackend::pollMembership() {
    if (rlnPolling_ || rlnPhase_.isEmpty() || rlnPhase_ == QLatin1String("missing")) return;
    rlnPolling_ = true;
    const auto again = [this]() {
        rlnPolling_ = false;
        // Often until it is usable, then now and then to notice a lapse.
        const bool settled = rlnPhase_ == QLatin1String("active") || rlnPhase_ == QLatin1String("quota");
        rlnTimer_.start(settled ? 5 * 60 * 1000 : 15000);
    };
    const auto ask = [this, again]() {
        auto* client = modules().api ? modules().api->getClient(QStringLiteral("liblogos_rln_module")) : nullptr;
        if (!client) {
            rlnPolling_ = false;
            setRln(QStringLiteral("missing"), QStringLiteral("no client for liblogos_rln_module"));
            return;
        }
        // Every membership this node holds on the registry, read rather than
        // get_membership_state: once a sponsored request has failed, the
        // failed record and its successor make the state call ambiguous.
        client->invokeRemoteMethodAsync(
            QStringLiteral("liblogos_rln_module"), QStringLiteral("get_memberships"),
            QVariantList{rlnRegistry_}, [this, client, again](QVariant v) {
                const QJsonObject o = replyObject(v);
                bool asked = false;
                guarded("rln memberships", [&]() {
                    if (o.contains(QLatin1String("memberships")) && !gifterPeer_.isEmpty()) {
                        membershipsAnswered(o);
                        return;
                    }
                    // Sponsor off, or no answer yet: the module's own view,
                    // with what its provisioning waits for.
                    asked = true;
                    client->invokeRemoteMethodAsync(
                        QStringLiteral("liblogos_rln_module"), QStringLiteral("get_membership_state"),
                        QVariantList{rlnRegistry_, rlnIdentifier_}, [this, again](QVariant v2) {
                            guarded("rln membership state", [&]() { membershipAnswered(replyObject(v2)); });
                            again();
                        });
                });
                if (!asked) again();
            });
    };
    if (!rlnRegistry_.isEmpty()) return ask();
    // Which registry and application scope the node proves against: delivery
    // knows, from its preset.
    modules().delivery_module.rlnStateAsync([this, ask](LogosResult r) {
        const QJsonObject o = replyObject(r.value);
        rlnRegistry_ = o.value(QLatin1String("registryId")).toString();
        rlnIdentifier_ = o.value(QLatin1String("rlnIdentifier")).toString();
        if (rlnRegistry_.isEmpty()) {
            rlnPolling_ = false;
            log("rln: delivery named no registry (" + s(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))) + ")");
            rlnTimer_.start(15000);
            return;
        }
        log("rln registry " + s(rlnRegistry_) + ", scope " + s(rlnIdentifier_));
        ask();
    });
}

void LogosForumBackend::membershipsAnswered(const QJsonObject& o) {
    // The records that back delivery's scope: registered for it, or for no
    // scope at all (registry-wide).
    bool usable = false, pending = false;
    QString failedReason, lapsedState;
    qint64 newestFailed = -1;
    for (const auto& v : o.value(QLatin1String("memberships")).toArray()) {
        const QJsonObject m = v.toObject();
        const QString scope = m.value(QLatin1String("rln_identifier")).toString();
        if (!scope.isEmpty() && scope.compare(rlnIdentifier_, Qt::CaseInsensitive) != 0) continue;
        const QString st = m.value(QLatin1String("state")).toString();
        if (st == QLatin1String("active") || st == QLatin1String("grace_period")) {
            usable = true;
            rlnRate_ = m.value(QLatin1String("rate_limit")).toInt(rlnRate_);
        } else if (st == QLatin1String("pending")) {
            pending = true;
        } else if (st == QLatin1String("failed")) {
            const qint64 at = static_cast<qint64>(m.value(QLatin1String("submitted_at")).toDouble());
            if (at > newestFailed) {
                newestFailed = at;
                failedReason = m.value(QLatin1String("failed_reason")).toString();
            }
        } else if (!st.isEmpty()) {
            lapsedState = st;
        }
    }
    if (usable) {
        giftFailures_ = 0;
        giftRetryAt_ = 0;
        if (rlnPhase_ != QLatin1String("quota")) setRln(QStringLiteral("active"), QString());
        return;
    }
    if (pending) return setRln(QStringLiteral("pending"), QString());
    if (gifting_) return;
    // Nothing usable: ask the sponsor, now or at the next retry.
    if (!failedReason.isEmpty() && rlnPhase_ == QLatin1String("pending")) {
        // The request this node made went through and then failed.
        if (failedReason.startsWith(QLatin1String("gifter_failed: "))) failedReason = failedReason.mid(15);
        return giftFailed(failedReason);
    }
    if (!lapsedState.isEmpty() && rlnPhase_ == QLatin1String("active"))
        log("rln: the membership is " + s(lapsedState) + "; asking the sponsor for a new one");
    if (giftRetryAt_ > static_cast<qint64>(now_ms())) {
        if (rlnPhase_ != QLatin1String("gift-failed")) setRln(QStringLiteral("gift-failed"), giftError_);
        else publishRln();  // the countdown
        return;
    }
    requestGift();
}

QString LogosForumBackend::retryMembership() {
    if (rlnPhase_ != QLatin1String("gift-failed")) return QStringLiteral("nothing to retry");
    if (gifterPeer_.isEmpty()) return QStringLiteral("the sponsor is off");
    giftRetryAt_ = 0;
    requestGift();
    return {};
}

void LogosForumBackend::requestGift() {
    if (gifting_ || gifterPeer_.isEmpty() || rlnRegistry_.isEmpty()) return;
    gifting_ = true;
    log("rln: asking the sponsor " + s(gifterPeer_).substr(0, 16) + "… for a membership");
    setRln(QStringLiteral("gifting"), QString());
    if (libp2pUp_) return giftRegister();
    // The request goes out through this node's own libp2p node: a plain one,
    // dialling out only. libp2p_module's replies are relayed by
    // rln_gifter_module.libp2p_call (they do not cross a module client as
    // values), and "already" means another app brought it up first.
    const QString cfg = QStringLiteral(
        "{\"addrs\":[\"/ip4/127.0.0.1/tcp/0\"],\"transport\":\"tcp\",\"maxConnections\":16,"
        "\"maxInConnections\":8,\"maxOutConnections\":8,\"maxConnsPerPeer\":1,"
        "\"mountGossipsub\":false,\"mountKad\":false,\"mountServiceDiscovery\":false}");
    giftLibp2p(QStringLiteral("createNode"), cfg, 0, [this](QString err) {
        if (!err.isEmpty()) return giftFailed(QStringLiteral("could not start the peer-to-peer node (%1)").arg(err));
        giftLibp2p(QStringLiteral("start"), QString(), 0, [this](QString err2) {
            if (!err2.isEmpty()) return giftFailed(QStringLiteral("could not start the peer-to-peer node (%1)").arg(err2));
            libp2pUp_ = true;
            giftRegister();
        });
    });
}

void LogosForumBackend::giftLibp2p(const QString& method, const QString& arg, int attempt,
                                   std::function<void(QString)> done) {
    auto* client = modules().api ? modules().api->getClient(QStringLiteral("rln_gifter_module")) : nullptr;
    if (!client) {
        gifting_ = false;
        setRln(QStringLiteral("missing"), QStringLiteral("rln_gifter_module is not loaded."));
        return;
    }
    QJsonArray args;
    if (!arg.isEmpty()) args.append(arg);
    const QString call = QString::fromUtf8(QJsonDocument(QJsonObject{{"method", method}, {"args", args}}).toJson(QJsonDocument::Compact));
    client->invokeRemoteMethodAsync(
        QStringLiteral("rln_gifter_module"), QStringLiteral("libp2p_call"), QVariantList{call},
        [this, method, arg, attempt, done](QVariant v) {
            // {"success","value","error"}, possibly as a JSON string in a
            // string. Not replyObject: it unwraps "value", null on success.
            QByteArray raw = v.typeId() == QMetaType::QByteArray ? v.toByteArray() : v.toString().toUtf8();
            QJsonObject o;
            for (int i = 0; i < 2 && o.isEmpty(); ++i) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isObject()) o = doc.object();
                else if (raw.startsWith('"')) raw = QJsonDocument::fromJson("[" + raw + "]").array().at(0).toString().toUtf8();
                else break;
            }
            if (o.isEmpty() && v.typeId() == QMetaType::QVariantMap) o = QJsonObject::fromVariantMap(v.toMap());
            QString err = o.value(QLatin1String("error")).toString();
            if (o.isEmpty()) err = QStringLiteral("no reply");
            if (o.value(QLatin1String("success")).toBool(false)) err.clear();
            if (err.contains(QLatin1String("already"))) err.clear();
            // The first calls can race libp2p_module's start-up.
            const bool racy = err.contains(QLatin1String("token")) || err.contains(QLatin1String("Invalid response"))
                || err.contains(QLatin1String("not recognized")) || err.contains(QLatin1String("not connected"))
                || err == QLatin1String("no reply");
            if (!err.isEmpty() && racy && attempt < 8) {
                QTimer::singleShot(1500, this, [this, method, arg, attempt, done]() { giftLibp2p(method, arg, attempt + 1, done); });
                return;
            }
            log("rln: libp2p " + s(method) + (err.isEmpty() ? " ok" : ": " + s(err)));
            done(err);
        });
}

void LogosForumBackend::giftRegister() {
    auto* client = modules().api ? modules().api->getClient(QStringLiteral("liblogos_rln_module")) : nullptr;
    if (!client) {
        gifting_ = false;
        setRln(QStringLiteral("missing"), QStringLiteral("no client for liblogos_rln_module"));
        return;
    }
    // Delegated registration (RLN Membership Allocation Protocol): the RLN
    // module makes the identity, keeps its secret, and sends only the
    // commitment to the sponsor through rln_gifter_module; the sponsor
    // registers it and pays. The membership is then this node's like any
    // other, and delivery proves with it.
    const QJsonArray opts{
        QJsonObject{{"key", "rate_limit"}, {"value", kGiftRate}},
        QJsonObject{{"key", "delegated"}, {"value", "true"}},
        QJsonObject{{"key", "gifter_peer_id"}, {"value", gifterPeer_}},
        QJsonObject{{"key", "gifter_multiaddr"}, {"value", gifterAddr_}},
    };
    client->invokeRemoteMethodAsync(
        QStringLiteral("liblogos_rln_module"), QStringLiteral("register_membership"),
        QVariantList{rlnRegistry_, rlnIdentifier_, QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact))},
        [this](QVariant v) {
            guarded("rln sponsored registration", [&]() {
                const QJsonObject o = replyObject(v);
                const QString err = o.isEmpty() ? QStringLiteral("the RLN module did not answer") : rlnError(o);
                if (!err.isEmpty()) return giftFailed(err);
                gifting_ = false;
                const QString st = o.value(QLatin1String("state")).toString();
                log("rln: sponsored membership requested, " + s(st.isEmpty() ? QStringLiteral("pending") : st)
                    + " (commitment " + s(o.value(QLatin1String("credential")).toObject()
                                             .value(QLatin1String("identity_commitment")).toString().left(16)) + "…)");
                if (st == QLatin1String("active") || st == QLatin1String("grace_period")) setRln(QStringLiteral("active"), st);
                else setRln(QStringLiteral("pending"), QString());
                rlnTimer_.start(15000);
            });
        }, Timeout(60000));
}

void LogosForumBackend::giftFailed(const QString& reason) {
    gifting_ = false;
    // The sponsor's account could not pay (the sequencer refused its fee).
    const QString why = reason.contains(QLatin1String("could not submit the registration"))
        ? QStringLiteral("The sponsor cannot pay for a membership right now (%1)").arg(reason)
        : reason;
    ++giftFailures_;
    static const int backoffMin[] = {1, 3, 10, 30};
    const int minutes = backoffMin[std::min(giftFailures_, 4) - 1];
    giftRetryAt_ = static_cast<qint64>(now_ms()) + minutes * 60000ll;
    giftError_ = why;
    log("rln: sponsored membership failed (" + s(why) + "); retry in " + std::to_string(minutes) + " min");
    setRln(QStringLiteral("gift-failed"), why);
    rlnTimer_.start(15000);
}

void LogosForumBackend::membershipAnswered(const QJsonObject& o) {
    if (o.isEmpty()) {
        setRln(QStringLiteral("wallet"), QString());
        return;
    }
    const QJsonObject err = o.value(QLatin1String("error")).toObject();
    if (!err.isEmpty()) {
        const QString cls = err.value(QLatin1String("class")).toString();
        const QString msg = err.value(QLatin1String("message")).toString();
        log("rln membership state: " + s(cls) + ": " + s(msg));
        if (cls == QLatin1String("permanent")) setRln(QStringLiteral("failed"), msg);
        else if (rlnPhase_ != QLatin1String("active")) setRln(QStringLiteral("wallet"), QString());
        return;
    }
    const QString state = o.value(QLatin1String("state")).toString();
    rlnRate_ = o.value(QLatin1String("rate_limit")).toInt(rlnRate_);
    if (state == QLatin1String("active") || state == QLatin1String("grace_period")) {
        if (rlnPhase_ != QLatin1String("quota")) setRln(QStringLiteral("active"), state);
        return;
    }
    if (state == QLatin1String("pending")) return setRln(QStringLiteral("pending"), QString());
    if (state != QLatin1String("unknown") && !state.isEmpty()) return setRln(QStringLiteral("lapsed"), state);
    // No membership yet: what the module's own provisioning is waiting on.
    const QJsonObject prov = o.value(QLatin1String("provisioning")).toObject();
    const QString step = prov.value(QLatin1String("step")).toString();
    const QString detail = prov.value(QLatin1String("detail")).toString();
    if (step == QLatin1String("awaiting_funding")) {
        // "<payer> needs <n> native (<price> price + <reserve> fee reserve); it holds <m>"
        static const QRegularExpression re(
            QStringLiteral("^([0-9a-fA-F]{64}) needs ([0-9]+) native \\(([0-9]+) price \\+ ([0-9]+) fee reserve\\)(?:; it holds ([0-9]+))?"));
        const auto m = re.match(detail);
        if (m.hasMatch()) {
            rlnPayer_ = m.captured(1);
            rlnNeeds_ = m.captured(2);
            rlnPrice_ = m.captured(3);
            rlnHolds_ = m.captured(5);
        }
        setRln(QStringLiteral("funding"), detail);
    } else if (step == QLatin1String("registering") || step == QLatin1String("done")) {
        setRln(QStringLiteral("registering"), detail);
    } else if (step == QLatin1String("refused")) {
        setRln(QStringLiteral("failed"), detail);
    } else {
        setRln(QStringLiteral("wallet"), detail);
    }
}

void LogosForumBackend::pump() {
    if (!engine_) return;
    guarded("pump", [&]() {
        engine_->tick(now_ms());  // lets a pending history answer go out once its wait has passed
        if (engine_->pump(now_ms()) > 0 || outboxCount() != static_cast<int>(store_->outbox().size())) publishOutbox();
    });
}

void LogosForumBackend::publishOutbox() {
    if (store_) setOutboxCount(static_cast<int>(store_->outbox().size()));
}

// ─── Posting ────────────────────────────────────────────────────────────────

Account* LogosForumBackend::selected() {
    for (auto& a : accounts_)
        if (q(a.label) == selectedLabel_) return &a;
    return nullptr;
}

QString LogosForumBackend::compose(bool topic, const QString& target, const QString& text, const QString& title,
                                   int mode, const QString& alias) {
    if (!engine_) return QStringLiteral("error: the forum is still starting");
    if (mode < 0 || mode > 2) return QStringLiteral("error: unknown posting mode");
    if (topic && title.trimmed().isEmpty()) return QStringLiteral("error: a topic needs a title");
    if (text.trimmed().isEmpty()) return QStringLiteral("error: nothing to post");
    if (mode == 1 && alias.trimmed().isEmpty()) return QStringLiteral("error: choose an alias");
    if (text.toUtf8().size() > static_cast<int>(forum::kMaxBody)) return QStringLiteral("error: the post is too long");
    if (title.toUtf8().size() > static_cast<int>(forum::kMaxTitle)) return QStringLiteral("error: the title is too long");
    if (alias.toUtf8().size() > static_cast<int>(forum::kMaxAlias)) return QStringLiteral("error: the alias is too long");

    Account* as = nullptr;
    if (mode != 2) {
        as = selected();
        if (!as) return QStringLiteral("error: no account selected");
        if (rotation_.due(*as, now_ms())) {
            forum::rotate(*as, now_ms());
            store_->save_account(*as, true);
            store_->scrub();
            log("rotated account " + as->label + " by policy");
        }
    }
    const std::string a = mode == 1 ? s(alias.trimmed()) : std::string();
    const std::string id = topic ? engine_->post_topic(as, s(title.trimmed()), s(text), a, now_ms())
                                 : engine_->post_reply(as, s(target), s(text), a, now_ms());
    if (id.empty()) return QStringLiteral("error: the post could not be saved on this device");
    if (as) {
        store_->save_account(*as, true);
        publishAccounts();
    }
    publishOutbox();
    QTimer::singleShot(0, this, [this]() { pump(); });
    return q(id);
}

QString LogosForumBackend::createTopic(QString title, QString body, int mode, QString alias) {
    return compose(true, QString(), body, title, mode, alias);
}

QString LogosForumBackend::reply(QString topicId, QString body, int mode, QString alias) {
    if (topicId.isEmpty()) return QStringLiteral("error: no topic");
    return compose(false, topicId, body, QString(), mode, alias);
}

// ─── Reading ────────────────────────────────────────────────────────────────

namespace {

QJsonObject postJson(const Post& p, const std::string& id, const std::vector<Account>& mine,
                     const QHash<QString, int>& outbox) {
    QString author, state;
    bool own = false;
    for (const auto& a : mine)
        if (a.key.pk == p.author) own = true;
    switch (p.mode) {
    case Mode::Anonymous: author = QStringLiteral("anonymous"); break;
    case Mode::Alias: author = q(p.alias); break;
    case Mode::Identity: {
        author = q(forum::short_key(p.author));
        for (const auto& a : mine)
            if (a.key.pk == p.author) author = q(a.label);
        break;
    }
    }
    const auto it = outbox.constFind(q(id));
    if (it != outbox.constEnd()) state = it.value() == 0 ? QStringLiteral("sending") : QStringLiteral("retrying");
    return QJsonObject{
        {"id", q(id)},
        {"title", q(p.title)},
        {"body", q(p.body)},
        {"author", author},
        {"authorKey", q(forum::to_hex(p.author.data(), p.author.size()))},
        {"mode", p.mode == Mode::Identity ? "identity" : p.mode == Mode::Alias ? "alias" : "anonymous"},
        {"ts", static_cast<double>(p.ts_ms)},
        {"mine", own},
        {"state", state},
    };
}

QString dump(const QJsonDocument& d) { return QString::fromUtf8(d.toJson(QJsonDocument::Compact)); }

} // namespace

QString LogosForumBackend::listTopics() {
    if (!store_) return QStringLiteral("[]");
    QHash<QString, int> outbox;
    for (const auto& o : store_->outbox()) outbox.insert(q(o.id), o.attempts);
    QJsonArray out;
    for (const auto& t : store_->topics(s(forumName()))) {
        // Kept from before dates were checked: not pinned above everything.
        if (t.topic.ts_ms > now_ms() + forum::Engine::kMaxClockSkewMs) continue;
        QJsonObject o = postJson(t.topic, t.id, accounts_, outbox);
        o.insert("replies", t.replies);
        o.insert("last", static_cast<double>(t.last_activity_ms));
        // New to this user: activity since they last opened the topic, and
        // never for what they wrote themselves as the latest word.
        const double seen = readUpTo_.value(q(t.id), readSince_);
        o.insert("unread", static_cast<double>(t.last_activity_ms) > seen && !(o.value("mine").toBool() && t.replies == 0));
        out.append(o);
    }
    return dump(QJsonDocument(out));
}

QString LogosForumBackend::thread(QString topicId) {
    if (!store_) return QStringLiteral("{}");
    QHash<QString, int> outbox;
    for (const auto& o : store_->outbox()) outbox.insert(q(o.id), o.attempts);
    QJsonObject out;
    const auto t = store_->get(s(topicId));
    out.insert("topic", t && t->kind == Kind::Topic ? QJsonValue(postJson(*t, s(topicId), accounts_, outbox))
                                                     : QJsonValue(QJsonValue::Null));
    QJsonArray replies;
    for (const auto& r : store_->replies(s(topicId))) replies.append(postJson(r, r.id(), accounts_, outbox));
    out.insert("replies", replies);
    return dump(QJsonDocument(out));
}

QString LogosForumBackend::markRead(QString topicId) {
    if (!store_) return QString();
    double last = 0;
    for (const auto& t : store_->topics(s(forumName())))
        if (q(t.id) == topicId) last = static_cast<double>(t.last_activity_ms);
    readUpTo_.insert(topicId, last);
    saveSettingsSoon();
    return QString();
}

void LogosForumBackend::countActivity(const std::string& payload, bool newPost) {
    if (newPost) ++actPosts_;
    if (payload.find("\"logos-forum-want\"") == std::string::npos) return;
    const auto j = nlohmann::json::parse(payload, nullptr, false);
    if (!j.is_object() || str_at(j, "forum") != s(forumName())) return;
    if (engine_ && engine_->is_own_request(str_at(j, "id"))) return;   // our own, echoed back
    ++actRequests_;
    const auto have = j.find("have");
    if (have != j.end() && have->is_number_integer() && have->get<int64_t>() == 0) ++actFresh_;
}

void LogosForumBackend::logActivity() {
    // Every node online asks for history about every five minutes, so twelve
    // requests an hour are one node; a request from a node holding nothing is
    // a fresh install.
    log("activity (last hour, other nodes): " + std::to_string(actRequests_) + " history requests, about " +
        std::to_string((actRequests_ + 6) / 12) + " node(s) online on average, " + std::to_string(actFresh_) +
        " fresh install(s), " + std::to_string(actPosts_) + " new post(s)");
    actRequests_ = actFresh_ = actPosts_ = 0;
}

void LogosForumBackend::saveSettingsSoon() {
    if (settingsScheduled_) return;
    settingsScheduled_ = true;
    QTimer::singleShot(2000, this, [this]() {
        settingsScheduled_ = false;
        saveSettings();
    });
}

void LogosForumBackend::postArrivedSoon(const QString& id, const QString& topicId) {
    arrivals_.insert(topicId, id);
    if (arrivalsScheduled_) return;
    arrivalsScheduled_ = true;
    QTimer::singleShot(kArrivalBatchMs, this, [this]() {
        arrivalsScheduled_ = false;
        // A few: one signal each, so an open topic refreshes and is marked read.
        // Many (an import): one signal with no topic, and the view refreshes once.
        if (arrivals_.size() <= 3)
            for (auto it = arrivals_.begin(); it != arrivals_.end(); ++it) emit postArrived(it.value(), it.key());
        else
            emit postArrived(QString(), QString());
        arrivals_.clear();
    });
}

// ─── Accounts ───────────────────────────────────────────────────────────────

void LogosForumBackend::publishAccounts() {
    QJsonArray arr;
    for (const auto& a : accounts_)
        arr.append(QJsonObject{{"label", q(a.label)},
                               {"key", q(forum::short_key(a.key.pk))},
                               {"posts", static_cast<int>(a.posts)},
                               {"created", static_cast<double>(a.created_ms)},
                               {"selected", q(a.label) == selectedLabel_}});
    setAccountsJson(dump(QJsonDocument(arr)));
    const Account* sel = selected();
    setMyLabel(sel ? q(sel->label) : QString());
    setMyKey(sel ? q(forum::short_key(sel->key.pk)) : QString());
    setRotationJson(dump(QJsonDocument(QJsonObject{
        {"maxPosts", static_cast<int>(rotation_.max_posts)},
        {"maxDays", static_cast<int>(rotation_.max_age_ms / 86400000ull)}})));
}

QString LogosForumBackend::createAccount(QString label) {
    if (!store_) return QStringLiteral("the forum is still starting");
    label = label.trimmed();
    if (label.isEmpty()) {
        for (int n = static_cast<int>(accounts_.size()) + 1;; ++n) {
            label = QStringLiteral("Account %1").arg(n);
            bool taken = false;
            for (const auto& a : accounts_) taken = taken || q(a.label) == label;
            if (!taken) break;
        }
    }
    for (const auto& a : accounts_)
        if (q(a.label) == label) return QStringLiteral("an account called \"%1\" already exists").arg(label);
    Account a{s(label), forum::Keypair::generate(), now_ms(), 0};
    store_->save_account(a, true);
    accounts_.push_back(a);
    selectedLabel_ = label;
    publishAccounts();
    return QString();
}

QString LogosForumBackend::selectAccount(QString label) {
    for (const auto& a : accounts_)
        if (q(a.label) == label) {
            selectedLabel_ = label;
            store_->save_account(a, true);
            publishAccounts();
            return QString();
        }
    return QStringLiteral("no such account");
}

QString LogosForumBackend::deleteAccount(QString label) {
    if (accounts_.size() <= 1) return QStringLiteral("keep at least one account");
    auto it = std::find_if(accounts_.begin(), accounts_.end(), [&](const Account& a) { return q(a.label) == label; });
    if (it == accounts_.end()) return QStringLiteral("no such account");
    sodium_memzero(it->key.sk.data(), it->key.sk.size());
    store_->remove_account(it->label);
    store_->scrub();
    accounts_.erase(it);
    if (!selected()) {
        selectedLabel_ = q(accounts_.front().label);
        store_->save_account(accounts_.front(), true);
    }
    publishAccounts();
    return QString();
}

QString LogosForumBackend::rotateAccount() {
    Account* a = selected();
    if (!a) return QStringLiteral("no account selected");
    forum::rotate(*a, now_ms());
    store_->save_account(*a, true);
    store_->scrub();
    publishAccounts();
    return QString();
}

QString LogosForumBackend::setRotation(int maxPosts, int maxDays) {
    if (maxPosts < 0 || maxDays < 0) return QStringLiteral("limits cannot be negative");
    rotation_.max_posts = static_cast<uint32_t>(maxPosts);
    rotation_.max_age_ms = static_cast<uint64_t>(maxDays) * 86400000ull;
    saveSettings();
    publishAccounts();
    return QString();
}

// ─── Settings ───────────────────────────────────────────────────────────────

void LogosForumBackend::loadSettings() {
    bool needSave = false;
    QFile f(dataDir() + QStringLiteral("/settings.json"));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    rotation_.max_posts = static_cast<uint32_t>(o.value("rotateAfterPosts").toInt(0));
    rotation_.max_age_ms = static_cast<uint64_t>(o.value("rotateAfterDays").toInt(0)) * 86400000ull;
    // A stable storage port per install, so the address this node announces
    // for its snapshots stays valid across restarts; random per install so two
    // Basecamp instances on one machine never collide.
    storagePort_ = o.value("storagePort").toInt(0);
    if (storagePort_ <= 0) {
        storagePort_ = 20000 + static_cast<int>(QRandomGenerator::global()->bounded(20000));
        needSave = true;
    }
    const QJsonObject read = o.value("readUpTo").toObject();
    for (auto it = read.begin(); it != read.end(); ++it) readUpTo_.insert(it.key(), it.value().toDouble());
    // A first launch has read nothing, and a forum full of "new" is noise:
    // what was there before the first launch counts as seen, what comes after
    // does not.
    fetchSnapshots_ = o.value("fetchSnapshots").toBool(false);
    readSince_ = o.value("readSince").toDouble(0);
    if (readSince_ <= 0) {
        readSince_ = static_cast<double>(now_ms());
        needSave = true;
    }
    std::vector<std::string> peers;
    for (const auto& v : o.value("storePeers").toArray())
        if (!v.toString().isEmpty()) peers.push_back(s(v.toString()));
    if (!peers.empty()) net_->set_store_peers(peers);
    QStringList shown;
    for (const auto& p : net_->store_peers()) shown << q(p);
    setStorePeers(shown.join('\n'));
    if (needSave) saveSettings();
}

QJsonObject LogosForumBackend::readJson() const {
    QJsonObject o;
    for (auto it = readUpTo_.begin(); it != readUpTo_.end(); ++it) o.insert(it.key(), it.value());
    return o;
}

void LogosForumBackend::saveSettings() {
    QJsonArray peers;
    const auto defaults = DeliveryTransport::default_store_peers(s(preset_));
    if (net_ && net_->store_peers() != defaults)
        for (const auto& p : net_->store_peers()) peers.append(q(p));
    const QJsonObject o{{"rotateAfterPosts", static_cast<int>(rotation_.max_posts)},
                        {"rotateAfterDays", static_cast<int>(rotation_.max_age_ms / 86400000ull)},
                        {"storePeers", peers},
                        {"storagePort", storagePort_},
                        {"readUpTo", readJson()},
                        {"fetchSnapshots", fetchSnapshots_},
                        {"readSince", readSince_}};
    QJsonObject out = o;
    if (!networkSetting_.isEmpty()) out.insert(QStringLiteral("network"), networkSetting_);  // the user's choice, kept
    if (!gifterSetting_.isEmpty()) out.insert(QStringLiteral("gifter"), gifterSetting_);
    QFile f(dataDir() + QStringLiteral("/settings.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(out).toJson());
}

QString LogosForumBackend::useStorePeers(QString peers) {
    if (!net_) return QStringLiteral("the forum is still starting");
    std::vector<std::string> list;
    // Split by hand: a QRegularExpression here would JIT-compile inside
    // Basecamp's hardened runtime, which has no allow-jit and traps.
    QStringList parts;
    QString cur;
    for (const QChar c : peers) {
        if (c.isSpace()) {
            if (!cur.isEmpty()) parts << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) parts << cur;
    for (const auto& p : parts) {
        if (!p.startsWith('/')) return QStringLiteral("not a multiaddr: %1").arg(p);
        list.push_back(s(p));
    }
    net_->set_store_peers(list.empty() ? DeliveryTransport::default_store_peers(s(preset_)) : list);
    QStringList shown;
    for (const auto& p : net_->store_peers()) shown << q(p);
    setStorePeers(shown.join('\n'));
    saveSettings();
    return QString();
}

// ─── History: Store queries and Logos Storage snapshots ─────────────────────

void LogosForumBackend::publishHistory() {
    QStringList parts;
    if (!lastCatchUp_.isEmpty()) parts << lastCatchUp_;
    if (!lastSnapshot_.isEmpty()) parts << lastSnapshot_;
    parts << (storageReady_ ? QStringLiteral("Storage ready") : QStringLiteral("Storage not ready"));
    setHistoryStatus(parts.join(QStringLiteral(" · ")));
}

void LogosForumBackend::runCatchUp(const char* why) {
    if (!engine_ || catchingUp_) return;
    catchingUp_ = true;
    log(std::string("catch-up starting (") + why + ")");
    lastCatchUp_ = QStringLiteral("Fetching missed posts…");
    publishHistory();
    const std::string reason = why;
    net_->history_async(s(contentTopic()), [this, reason](std::vector<std::string> payloads) {
        catchingUp_ = false;
        if (!engine_) return;
        int added = 0;
        for (const auto& m : payloads)
            if (engine_->receive(m, now_ms())) ++added;
        const std::string& peer = net_->last_history_peer();
        log("catch-up (" + reason + "): " + std::to_string(payloads.size()) + " messages, " + std::to_string(added) +
            " new, peer " + (peer.empty() ? "none — " + net_->last_history_error() : peer) + ", status " +
            net_->last_history_status());
        const std::string& err = net_->last_history_error();
        const QString why = err.find("DIAL") != std::string::npos ? QStringLiteral("no store node reachable")
                                                                    : q(err.substr(0, 60));
        lastCatchUp_ = peer.empty()
            ? QStringLiteral("History: %1, asking peers").arg(why)
            : QStringLiteral("Caught up %1 · %2 new").arg(hhmm(now_ms())).arg(added);
        publishHistory();
        // Store nodes may keep no archive at all; ask the peers too.
        QTimer::singleShot(0, this, [this, reason]() {
            // Over Storage first only when the user opted in; by default the
            // answer comes over Delivery relays and nobody learns our address.
            if (engine_) engine_->request_history(now_ms(), /*via_delivery=*/!g_fetchSnapshots);
            log("asked peers for history");
            // Just started: the node may not have found its relay peers yet, and
            // a request sent into an empty mesh is lost. Ask again shortly if
            // nothing has arrived, rather than waiting for the periodic catch-up.
            if (reason == "node started" || reason == "joined running node") {
                const size_t had = store_->count();
                for (int delay : {20000, 60000})
                    QTimer::singleShot(delay, this, [this, had]() {
                        if (!engine_ || store_->count() != had) return;
                        engine_->request_history(now_ms(), !g_fetchSnapshots);
                        log("asked peers for history again (nothing arrived yet)");
                    });
            }
        });
    });
}

QString LogosForumBackend::catchUp() {
    runCatchUp("asked");
    return QString();
}

void LogosForumBackend::wireStorage() {
    auto& st = modules().storage_module;
    st.onStorageStart([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        storageReady_ = bool_at(j, "success");
        log(std::string("storage start: ") + (storageReady_ ? "ok" : s(payload)));
        publishHistory();
        if (storageReady_) QTimer::singleShot(0, this, [this]() {
            learnStorageIdentity();
            if (pendingAnnouncement_) {
                const forum::Announcement an = *pendingAnnouncement_;
                pendingAnnouncement_.reset();
                fetchSnapshot(an);
            }
        });
    });
    st.onStorageUploadProgress([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object() || q(str_at(j, "sessionId")) != uploadSession_) return;
        armUploadWatchdog();
        log("upload progress: " + s(payload).substr(0, 200));
        if (!bool_at(j, "success")) {
            failUpload(QStringLiteral("Snapshot upload failed: %1").arg(q(str_at(j, "error"))));
            return;
        }
        QTimer::singleShot(0, this, [this]() {
            if (uploadOffset_ < uploadDoc_.size()) uploadNextChunk();
            else finishUpload();
        });
    });
    st.onStorageDownloadProgress([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!bool_at(j, "success") || q(str_at(j, "sessionId")) != downloadSession_)
            return;
        const std::string chunk = str_at(j, "chunk");
        if (chunk.empty()) return;
        downloadBuf_ += b64decode(chunk);
        if (downloadBuf_.size() > kMaxSnapshotBytes) {  // not a forum snapshot; stop
            modules().storage_module.downloadCancelAsync(downloadSession_, [](LogosResult) {});
            downloadSession_.clear();
            downloadBuf_.clear();
        }
    });
    st.onStorageDownloadManifestDone([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object() || q(str_at(j, "cid")) != manifestCid_ || !manifestNext_) return;
        auto next = manifestNext_;
        manifestNext_ = nullptr;
        if (!bool_at(j, "success")) {
            log("snapshot manifest not found: " + str_at(j, "error"));
            downloadSession_.clear();
            seenSnapshots_.erase(s(manifestCid_));
            historyOverDelivery("snapshot manifest not found");
            return;
        }
        log("snapshot manifest found; downloading");
        QTimer::singleShot(0, this, next);
    });
    st.onStorageDownloadDone([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object() || q(str_at(j, "sessionId")) != downloadSession_) return;
        downloadSession_.clear();
        log("download done: " + s(payload).substr(0, 200) + ", " + std::to_string(downloadBuf_.size()) + " bytes");
        if (bool_at(j, "success") && engine_) {
            int added = 0;
            guarded("snapshot import", [&]() { added = engine_->import_snapshot(downloadBuf_, now_ms()); });
            lastSnapshot_ = QStringLiteral("Loaded a snapshot · %1 new").arg(added);
            log("snapshot imported: " + std::to_string(added) + " new");
            publishHistory();
        } else {
            historyOverDelivery("snapshot download failed");
        }
        downloadBuf_.clear();
    });

    initStorage(true);
    publishHistory();
}

void LogosForumBackend::initStorage(bool withDiscPort) {
    auto& st = modules().storage_module;
    QJsonObject o{
        // Storage runs its own network. With snapshots over Mix it is
        // logos.test: storage_module 3.0.0's Mix relay list for logos.dev has
        // an entry with an empty key, and Storage then refuses to start. Every
        // node that opts in picks the same, so they find each other.
        // LOGOS_FORUM_STORAGE_NETWORK overrides.
        {"network", qEnvironmentVariable("LOGOS_FORUM_STORAGE_NETWORK",
                                         g_fetchSnapshots && g_snapshotsOverMix ? QStringLiteral("logos.test") : preset_)},
        {"data-dir", dataDir() + QStringLiteral("/storage")},
        {"listen-port", storagePort_}};
    // The 2.1 series has a fixed default discovery port (8090), so two
    // Basecamp instances on one machine would collide without an ephemeral
    // one; later libstorage (Kademlia) no longer has the option and refuses a
    // config that names it. Try with it, then without.
    if (withDiscPort) o.insert("disc-port", 0);
    // Snapshots over Mix: join the network's Mix (its configuration is filled
    // in by the module from the network name).
    if (g_fetchSnapshots && g_snapshotsOverMix) o.insert("mix-enabled", true);
    const QString cfg = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (ok(st.init(cfg))) {
        log(std::string("storage init ok") + (withDiscPort ? "" : " (without disc-port)"));
        if (!ok(st.start())) {
            log("storage start refused");
            lastSnapshot_ = QStringLiteral("Storage failed to start");
            publishHistory();
        }
        return;
    }
    if (withDiscPort) {
        initStorage(false);
        return;
    }
    // Refused twice: either another app already started the shared node (then
    // it answers), or this storage_module cannot run with our settings. Say
    // which, rather than stay "not ready" with no reason.
    log("storage init refused; checking whether a node is already running");
    modules().storage_module.debugAsync([this](LogosResult r) {
        storageReady_ = r.success;
        log(std::string("storage ") + (r.success ? "already running (started by another app)" : "failed to start: " + s(r.getError())));
        if (!r.success) lastSnapshot_ = QStringLiteral("Storage failed to start: %1").arg(r.getError());
        publishHistory();
        if (r.success) learnStorageIdentity();
    });
}

QString LogosForumBackend::saveSnapshot() {
    if (!engine_) return QStringLiteral("the forum is still starting");
    if (uploading_) return QStringLiteral("a snapshot is already being saved");
    uploading_ = true;
    armUploadWatchdog();
    uploadDoc_ = engine_->snapshot();
    uploadOffset_ = 0;
    uploadPosts_ = store_->count();
    lastSnapshot_ = QStringLiteral("Saving snapshot…");
    publishHistory();
    // Every storage call from here on is asynchronous: a synchronous call made
    // while storage_module is dispatching one of its own events never returns
    // before the RPC timeout.
    // advertise: a snapshot is offered to the peers who asked for it.
    modules().storage_module.uploadInitAsync(
        QStringLiteral("logos-forum-snapshot.json"), static_cast<int>(kUploadChunk), true, [this](LogosResult r) {
            if (!r.success) {
                failUpload(QStringLiteral("Storage unavailable: %1").arg(r.getError()));
                return;
            }
            uploadSession_ = r.getString();
            log("upload session " + s(uploadSession_) + ", " + std::to_string(uploadDoc_.size()) + " bytes");
            armUploadWatchdog();
            uploadNextChunk();
        });
    return QString();
}

void LogosForumBackend::failUpload(const QString& why) {
    log("snapshot upload failed: " + s(why));
    uploading_ = false;
    uploadSession_.clear();
    lastSnapshot_ = why;
    publishHistory();
}

void LogosForumBackend::uploadNextChunk() {
    // Cut on a character boundary: each chunk crosses as a QString, and half
    // a UTF-8 character would come back as U+FFFD and break its post's signature.
    const std::string chunk = uploadDoc_.substr(uploadOffset_, forum::utf8_cut(uploadDoc_, uploadOffset_, kUploadChunk));
    uploadOffset_ += chunk.size();
    armUploadWatchdog();
    // Completion arrives as a storageUploadProgress event for this session.
    modules().storage_module.uploadChunkAsync(uploadSession_, q(chunk), [this](LogosResult r) {
        if (!r.success) {
            modules().storage_module.uploadCancelAsync(uploadSession_, [](LogosResult) {});
            failUpload(QStringLiteral("Snapshot upload failed: %1").arg(r.getError()));
        }
    });
}

void LogosForumBackend::armUploadWatchdog() {
    // One per upload, re-armed at every step: init, each chunk, finalize. An
    // upload whose next step never answers is as good as a failed one, and
    // must not block snapshots until a restart.
    const int gen = ++uploadProgress_;
    QTimer::singleShot(kUploadWatchdogMs, this, [this, gen]() {
        if (!uploading_ || uploadProgress_ != gen) return;
        if (!uploadSession_.isEmpty()) modules().storage_module.uploadCancelAsync(uploadSession_, [](LogosResult) {});
        failUpload(QStringLiteral("Snapshot upload stalled"));
    });
}

void LogosForumBackend::finishUpload() {
    armUploadWatchdog();
    modules().storage_module.uploadFinalizeAsync(uploadSession_, [this](LogosResult r) {
        if (!r.success) {
            failUpload(QStringLiteral("Snapshot upload failed: %1").arg(r.getError()));
            return;
        }
        uploading_ = false;
        uploadSession_.clear();
        const std::string cid = s(r.getString());
        seenSnapshots_.insert(cid);  // our own; nothing to fetch
        postsAtLastSnapshot_ = uploadPosts_;
        lastSnapshotCid_ = cid;
        // Peers act only on an announcement that answers their own request.
        if (!answerRe_.empty())
            QTimer::singleShot(0, this, [this, cid]() {
                engine_->announce_snapshot(cid, uploadPosts_, now_ms(), answerRe_);
                answerRe_.clear();
            });
        lastSnapshot_ = QStringLiteral("Snapshot %1 saved %2").arg(q(cid.substr(0, 10)) + QStringLiteral("…"), hhmm(now_ms()));
        log("snapshot saved as " + cid + " (" + std::to_string(uploadPosts_) + " posts)");
        publishHistory();
    });
}

void LogosForumBackend::learnStorageIdentity() {
    // Announcements name this node as the snapshot's provider, so a peer can
    // dial it directly: on logos.test two nodes behind NAT do not find each
    // other's content through the DHT alone.
    modules().storage_module.debugAsync([this](LogosResult r) {
        if (!r.success || !engine_) return;
        QString text = r.value.toString();
        if (text.isEmpty()) text = QString::fromUtf8(QJsonDocument::fromVariant(r.value).toJson(QJsonDocument::Compact));
        const auto j = nlohmann::json::parse(s(text), nullptr, false);
        const std::string id = str_at(j, "id");
        std::vector<std::string> addrs;
        if (j.is_object() && j.contains("addrs") && j["addrs"].is_array())
            for (const auto& a : j["addrs"])
                if (a.is_string()) addrs.push_back(a.get<std::string>());
        if (addrs.empty() && storagePort_ > 0 && localPeersAllowed()) {
            // Nothing announced (no public address yet): offer what we listen on.
            for (const QHostAddress& h : QNetworkInterface::allAddresses())
                if (h.protocol() == QAbstractSocket::IPv4Protocol)
                    addrs.push_back("/ip4/" + s(h.toString()) + "/tcp/" + std::to_string(storagePort_));
        }
        // Only public addresses are announced: a LAN or loopback address says
        // something about this machine and helps nobody outside it.
        if (!localPeersAllowed())
            addrs.erase(std::remove_if(addrs.begin(), addrs.end(), [](const std::string& a) { return !forum::is_public_multiaddr(a); }),
                        addrs.end());
        engine_->set_storage_provider(id, addrs);
        std::string list;
        for (const auto& a : addrs) list += " " + a;
        log("storage peer " + id + " at" + list);
    });
}

void LogosForumBackend::historyOverDelivery(const char* why) {
    // Logos Storage could not bring the snapshot here — typically two storage
    // nodes behind NAT that cannot reach each other. Ask again, this time for
    // the posts themselves on the Delivery topic, which crosses NAT. At most
    // once a minute: the answer comes from whoever has it, not from retrying.
    if (!engine_ || now_ms() < lastDeliveryAsk_ + 60000) return;
    lastDeliveryAsk_ = now_ms();
    engine_->request_history(now_ms(), /*via_delivery=*/true);
    log(std::string(why) + ": asked peers to send history over Delivery");
    lastSnapshot_ = QStringLiteral("Getting history from peers over Delivery…");
    publishHistory();
}

void LogosForumBackend::fetchSnapshot(const forum::Announcement& an) {
    // Storage 3.0 can take half a minute to start: an answer that arrives
    // before it is ready is kept and fetched once it is.
    if (!storageReady_) {
        pendingAnnouncement_ = an;
        log("snapshot " + an.cid + " announced; fetching it once storage is ready");
        return;
    }
    // One download at a time, each announced snapshot once.
    if (!downloadSession_.isEmpty() || seenSnapshots_.count(an.cid)) return;
    seenSnapshots_.insert(an.cid);
    downloadSession_ = q(an.cid);  // storage_module names a download session by its CID
    downloadBuf_.clear();
    lastSnapshot_ = QStringLiteral("Loading a peer's snapshot…");
    publishHistory();
    const std::string cid = an.cid;
    // Over Mix by default: the download is tunnelled through relays, so the
    // provider never learns our address, and nothing is re-served
    // (advertise=false). The provider is then found through the DHT, not
    // dialled: dialling it directly is exactly what would reveal us.
    const bool viaMix = g_snapshotsOverMix;
    auto chunks = [this, cid, viaMix]() {
        log("fetching snapshot " + cid + (viaMix ? " over Mix" : ""));
        modules().storage_module.downloadChunksAsync(q(cid), false, kDownloadChunk, viaMix, false, [this, cid](LogosResult r) {
            if (!r.success) {
                log("snapshot " + cid + " unavailable: " + s(r.getError()));
                downloadSession_.clear();
                seenSnapshots_.erase(cid);  // a later announcement may succeed
                historyOverDelivery("snapshot unavailable");
                return;
            }
            downloadSession_ = r.getString();
        }, Timeout(120000));
    };
    // The manifest first, without blocking: finding it may take a DHT lookup
    // longer than a synchronous call waits (a direct downloadChunks gave up
    // after 30 s with "Failed to start chunk download"). Its event starts the
    // chunks (see storageDownloadManifestDone in wireStorage).
    manifestNext_ = chunks;
    manifestCid_ = q(cid);
    log("looking up the manifest of snapshot " + cid + (viaMix ? " over Mix" : ""));
    modules().storage_module.downloadManifestAsync(q(cid), viaMix, false, [this, cid](LogosResult r) {
        if (r.success) return;  // dispatched; the result comes as an event
        log("snapshot " + cid + " manifest lookup refused: " + s(r.getError()));
        manifestNext_ = nullptr;
        downloadSession_.clear();
        seenSnapshots_.erase(cid);
        historyOverDelivery("snapshot manifest unavailable");
    });
    // A download that never finishes is as good as a failed one.
    QTimer::singleShot(kDownloadWatchdogMs, this, [this, cid]() {
        if (downloadSession_ != q(cid)) return;
        modules().storage_module.downloadCancelAsync(downloadSession_, [](LogosResult) {});
        manifestNext_ = nullptr;
        downloadSession_.clear();
        downloadBuf_.clear();
        seenSnapshots_.erase(cid);
        historyOverDelivery("snapshot download timed out");
    });
    if (viaMix || an.peer.empty()) return;
    // Not over Mix (the user turned it off): dial the provider directly, public
    // addresses only, never into this machine or its LAN on the word of an
    // unsigned message.
    QStringList addrs;
    for (const auto& a : an.addrs)
        if (localPeersAllowed() || forum::is_public_multiaddr(a)) addrs << q(a);
    if (addrs.isEmpty()) return;
    log("dialing snapshot provider " + an.peer);
    modules().storage_module.connectAsync(q(an.peer), addrs, [this](LogosResult r) {
        if (!r.success) log("dial failed: " + s(r.getError()) + " — the DHT may still find it");
    });
}
