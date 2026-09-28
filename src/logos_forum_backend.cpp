// SPDX-License-Identifier: MIT OR Apache-2.0
#include "logos_forum_backend.h"

#include <algorithm>
#include <iostream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
constexpr int kPumpMs = 2000;
constexpr int kCatchUpMs = 5 * 60 * 1000;
constexpr int kSnapshotMs = 30 * 60 * 1000;
constexpr int kMaxSubscribeAttempts = 10;
constexpr int kSubscribeRetryMs = 3000;
constexpr size_t kUploadChunk = 48 * 1024;
constexpr int kDownloadChunk = 64 * 1024;
constexpr size_t kMaxSnapshotBytes = 32u * 1024 * 1024;

} // namespace

LogosForumBackend::LogosForumBackend() {
    setAppVersion(QStringLiteral(LOGOS_FORUM_VERSION));
    const QString name = qEnvironmentVariable("LOGOS_FORUM_NAME", QString::fromLatin1(kDefaultForum));
    setForumName(name);
    setContentTopic(q(forum::content_topic(s(name))));
}

LogosForumBackend::~LogosForumBackend() = default;

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
    QTimer::singleShot(0, [this]() { bootstrap(); });
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
    loadSettings();

    // The engine subscribes in its constructor; that first attempt may fail
    // while the node is not up yet, and subscribe() below retries.
    engine_ = std::make_unique<forum::Engine>(*store_, *net_, s(forumName()));
    engine_->on_post = [this](const Post& p, const std::string& id) {
        emit postArrived(q(id), q(p.kind == Kind::Topic ? id : p.topic_id));
    };
    engine_->on_sent = [this](const std::string& id, const std::string& request) {
        log("sent " + id.substr(0, 12) + " as request " + request);
        if (!request.empty()) inFlight_.insert(q(request), q(id));
        emit postStateChanged(q(id), QStringLiteral("sent"), QString());
    };
    engine_->on_snapshot = [this](const std::string& cid, size_t) { fetchSnapshot(cid); };

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

    connect(&pumpTimer_, &QTimer::timeout, [this]() { pump(); });
    pumpTimer_.start(kPumpMs);
    connect(&historyTimer_, &QTimer::timeout, [this]() { runCatchUp("periodic"); });
    historyTimer_.start(kCatchUpMs);
    connect(&snapshotTimer_, &QTimer::timeout, [this]() {
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
            QTimer::singleShot(0, [this]() {
                runCatchUp("reconnected");
                pump();
            });
    });
    d.on("messageReceived", [this](const QVariantList& data) {
        if (data.size() < 3 || !engine_) return;
        if (data.at(1).toString() != contentTopic()) return;  // another app's traffic
        const bool fresh = engine_->receive(data.at(2).toByteArray().toStdString(), now_ms());
        log(std::string("received ") + (fresh ? "a new post" : "a known or foreign message"));
    });
    d.on("nodeStarted", [this](const QVariantList& data) {
        const bool up = !data.isEmpty() && data.at(0).toBool();
        log(std::string("nodeStarted ") + (up ? "ok" : "failed"));
        if (!up) {
            setStatus(QStringLiteral("Node failed to start: %1").arg(data.value(1).toString()));
            return;
        }
        subscribeAttempts_ = 0;
        QTimer::singleShot(0, [this]() {
            subscribe();
            runCatchUp("node started");
        });
    });
    d.on("messageSent", [this](const QVariantList& data) {
        inFlight_.remove(data.value(0).toString());
    });
    d.on("messageError", [this](const QVariantList& data) {
        const QString request = data.value(0).toString();
        const auto it = inFlight_.constFind(request);
        if (it == inFlight_.constEnd()) return;  // not ours: delivery_module is shared
        const QString id = it.value();
        const QString why = data.value(2).toString();
        inFlight_.remove(request);
        // Accepted locally, then lost: back in the outbox, retried with back-off.
        if (engine_ && engine_->requeue(s(id), s(why), now_ms())) publishOutbox();
        emit postStateChanged(id, QStringLiteral("failed"), why);
    });
}

void LogosForumBackend::startNode() {
    // The layered config shape; bare node keys at top level would switch the
    // parser to the legacy shape and fixed ports (see forum-sample-app).
    const QString cfg = QStringLiteral(R"({"mode":"Core","preset":"logos.test"})");
    const LogosResult created = modules().delivery_module.createNode(cfg);
    if (!created.success) {
        // delivery_module is shared across Basecamp apps: another may already
        // run the node, in which case no nodeStarted will come for us.
        log("createNode refused (node already running?): " + s(created.getError()));
        subscribe();
        runCatchUp("joined running node");
        return;
    }
    subscribe();  // before start(), so nothing that arrives early is missed
    setStatus(QStringLiteral("Starting node…"));
    const LogosResult started = modules().delivery_module.start();
    if (!started.success) setStatus(QStringLiteral("Node failed to start: %1").arg(started.getError()));
}

void LogosForumBackend::subscribe() {
    if (subscribed_ || !net_) return;
    ++subscribeAttempts_;
    if (net_->subscribe(s(contentTopic()))) {
        log("subscribed on attempt " + std::to_string(subscribeAttempts_));
        subscribed_ = true;
        refreshStatus();
        return;
    }
    // Usually the node is still bootstrapping; that passes. Posting is not
    // affected either way — it runs off the local store.
    setStatus(QStringLiteral("Joining the forum topic… (attempt %1)").arg(subscribeAttempts_));
    if (subscribeAttempts_ < kMaxSubscribeAttempts)
        QTimer::singleShot(kSubscribeRetryMs, [this]() { subscribe(); });
}

void LogosForumBackend::refreshStatus() {
    if (!subscribed_) return;
    // A node with no peers still accepts sends; say so rather than "Connected".
    setStatus(connectionState_.isEmpty() ? QStringLiteral("Joined — waiting for peers") : connectionState_);
}

void LogosForumBackend::pump() {
    if (!engine_) return;
    if (engine_->pump(now_ms()) > 0 || outboxCount() != static_cast<int>(store_->outbox().size())) publishOutbox();
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
            log("rotated account " + as->label + " by policy");
        }
    }
    const std::string a = mode == 1 ? s(alias.trimmed()) : std::string();
    const std::string id = topic ? engine_->post_topic(as, s(title.trimmed()), s(text), a, now_ms())
                                 : engine_->post_reply(as, s(target), s(text), a, now_ms());
    if (as) {
        store_->save_account(*as, true);
        publishAccounts();
    }
    publishOutbox();
    QTimer::singleShot(0, [this]() { pump(); });
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
        QJsonObject o = postJson(t.topic, t.id, accounts_, outbox);
        o.insert("replies", t.replies);
        o.insert("last", static_cast<double>(t.last_activity_ms));
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
    QFile f(dataDir() + QStringLiteral("/settings.json"));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    rotation_.max_posts = static_cast<uint32_t>(o.value("rotateAfterPosts").toInt(0));
    rotation_.max_age_ms = static_cast<uint64_t>(o.value("rotateAfterDays").toInt(0)) * 86400000ull;
    std::vector<std::string> peers;
    for (const auto& v : o.value("storePeers").toArray())
        if (!v.toString().isEmpty()) peers.push_back(s(v.toString()));
    if (!peers.empty()) net_->set_store_peers(peers);
    QStringList shown;
    for (const auto& p : net_->store_peers()) shown << q(p);
    setStorePeers(shown.join('\n'));
}

void LogosForumBackend::saveSettings() {
    QJsonArray peers;
    const auto defaults = DeliveryTransport::default_store_peers();
    if (net_ && net_->store_peers() != defaults)
        for (const auto& p : net_->store_peers()) peers.append(q(p));
    const QJsonObject o{{"rotateAfterPosts", static_cast<int>(rotation_.max_posts)},
                        {"rotateAfterDays", static_cast<int>(rotation_.max_age_ms / 86400000ull)},
                        {"storePeers", peers}};
    QFile f(dataDir() + QStringLiteral("/settings.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson());
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
    net_->set_store_peers(list.empty() ? DeliveryTransport::default_store_peers() : list);
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
    parts << (storageReady_ ? QStringLiteral("Storage ready") : QStringLiteral("Storage starting…"));
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
            " new, peer " + (peer.empty() ? "none — " + net_->last_history_error() : peer));
        lastCatchUp_ = peer.empty()
            ? QStringLiteral("History unavailable (%1)").arg(q(net_->last_history_error()))
            : QStringLiteral("Caught up %1 · %2 new").arg(hhmm(now_ms())).arg(added);
        publishHistory();
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
    });
    st.onStorageUploadProgress([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object() || q(str_at(j, "sessionId")) != uploadSession_) return;
        if (!bool_at(j, "success")) {
            lastSnapshot_ = QStringLiteral("Snapshot upload failed");
            uploadSession_.clear();
            publishHistory();
            return;
        }
        if (uploadOffset_ < uploadDoc_.size()) uploadNextChunk();
        else finishUpload();
    });
    st.onStorageDownloadProgress([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!bool_at(j, "success") || q(str_at(j, "sessionId")) != downloadSession_)
            return;
        const std::string chunk = str_at(j, "chunk");
        if (chunk.empty()) return;
        downloadBuf_ += b64decode(chunk);
        if (downloadBuf_.size() > kMaxSnapshotBytes) {  // not a forum snapshot; stop
            modules().storage_module.downloadCancel(downloadSession_);
            downloadSession_.clear();
            downloadBuf_.clear();
        }
    });
    st.onStorageDownloadDone([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object() || q(str_at(j, "sessionId")) != downloadSession_) return;
        downloadSession_.clear();
        if (bool_at(j, "success") && engine_) {
            const int added = engine_->import_snapshot(downloadBuf_, now_ms());
            lastSnapshot_ = QStringLiteral("Loaded a snapshot · %1 new").arg(added);
            log("snapshot imported: " + std::to_string(added) + " new");
            publishHistory();
        }
        downloadBuf_.clear();
    });

    // Storage is shared across Basecamp apps: if another app has started it,
    // init is refused and the node is already usable.
    const QString cfg = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"network", "logos.test"},
        {"data-dir", dataDir() + QStringLiteral("/storage")},
        // Ephemeral ports: the default discovery port is fixed (8090), so two
        // Basecamp instances on one machine would otherwise collide.
        {"listen-port", 0},
        {"disc-port", 0}}).toJson(QJsonDocument::Compact));
    if (ok(st.init(cfg))) ok(st.start());
    else storageReady_ = true;
    publishHistory();
}

QString LogosForumBackend::saveSnapshot() {
    if (!engine_) return QStringLiteral("the forum is still starting");
    if (!uploadSession_.isEmpty()) return QStringLiteral("a snapshot is already being saved");
    uploadDoc_ = engine_->snapshot();
    uploadOffset_ = 0;
    uploadPosts_ = store_->count();
    const LogosResult r = modules().storage_module.uploadInit(QStringLiteral("logos-forum-snapshot.json"),
                                                              static_cast<int>(kUploadChunk));
    if (!r.success) {
        lastSnapshot_ = QStringLiteral("Storage unavailable: %1").arg(r.getError());
        publishHistory();
        return lastSnapshot_;
    }
    uploadSession_ = r.getString();
    lastSnapshot_ = QStringLiteral("Saving snapshot…");
    publishHistory();
    uploadNextChunk();
    return QString();
}

void LogosForumBackend::uploadNextChunk() {
    const std::string chunk = uploadDoc_.substr(uploadOffset_, kUploadChunk);
    uploadOffset_ += chunk.size();
    const LogosResult r = modules().storage_module.uploadChunk(uploadSession_, q(chunk));
    if (!r.success) {
        modules().storage_module.uploadCancel(uploadSession_);
        uploadSession_.clear();
        lastSnapshot_ = QStringLiteral("Snapshot upload failed: %1").arg(r.getError());
        publishHistory();
    }
}

void LogosForumBackend::finishUpload() {
    const LogosResult r = modules().storage_module.uploadFinalize(uploadSession_);
    uploadSession_.clear();
    if (!r.success) {
        lastSnapshot_ = QStringLiteral("Snapshot upload failed: %1").arg(r.getError());
        publishHistory();
        return;
    }
    const std::string cid = s(r.getString());
    seenSnapshots_.insert(cid);  // our own; nothing to fetch
    postsAtLastSnapshot_ = uploadPosts_;
    engine_->announce_snapshot(cid, uploadPosts_, now_ms());
    lastSnapshot_ = QStringLiteral("Snapshot %1 saved %2").arg(q(cid.substr(0, 10)) + QStringLiteral("…"), hhmm(now_ms()));
    log("snapshot saved as " + cid + " (" + std::to_string(uploadPosts_) + " posts)");
    publishHistory();
}

void LogosForumBackend::fetchSnapshot(const std::string& cid) {
    // One download at a time, each announced snapshot once.
    if (!downloadSession_.isEmpty() || !seenSnapshots_.insert(cid).second) return;
    const LogosResult r = modules().storage_module.downloadChunks(q(cid), false, kDownloadChunk);
    if (!r.success) {
        log("snapshot " + cid + " unavailable: " + s(r.getError()));
        return;
    }
    downloadSession_ = r.getString();
    downloadBuf_.clear();
    lastSnapshot_ = QStringLiteral("Loading a peer's snapshot…");
    publishHistory();
}
