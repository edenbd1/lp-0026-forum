// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Private group forums and the reader's moderation, on the Basecamp side.
// The rules live in core/ (group.h, moderation.h, intake.h) and are tested
// there; this file wires them to Logos Delivery and to the view.
//
// Each joined group runs its own forum::Engine on the shared store, behind a
// SealedTransport: whatever that engine sends (posts, history requests,
// bundles) is sealed before it reaches delivery_module, on the group's own
// topic. forum.log never names a group, its topic or its posts.
#include "logos_forum_backend.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <sodium.h>

#include "logos_sdk.h"
#include "logos_types.h"

void forumLog(const std::string& m);  // logos_forum_backend.cpp

namespace {

uint64_t nowMs() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }
QString q(const std::string& v) { return QString::fromStdString(v); }
std::string s(const QString& v) { return v.toStdString(); }
QString dumpJson(const QJsonDocument& d) { return QString::fromUtf8(d.toJson(QJsonDocument::Compact)); }

bool parseKey(const QString& hex, forum::PublicKey& out) {
    const std::string h = s(hex.trimmed());
    if (h.size() != 64) return false;
    size_t n = 0;
    return sodium_hex2bin(out.data(), out.size(), h.data(), h.size(), nullptr, &n, nullptr) == 0 && n == 32;
}

} // namespace

void LogosForumBackend::initGroups() {
    moderation_ = std::make_unique<forum::Moderation>(*store_);
    groups_ = std::make_unique<forum::Groups>(*store_);
    for (const auto& g : groups_->all()) attachGroup(g);
    if (!joined_.empty()) forumLog("private forums joined: " + std::to_string(joined_.size()));
    publishForums();
}

LogosForumBackend::Joined* LogosForumBackend::attachGroup(const forum::Group& g) {
    if (auto* j = joinedById(g.forum_id())) return j;
    auto j = std::make_unique<Joined>();
    j->group = g;
    j->net = std::make_unique<forum::SealedTransport>(*net_, g);
    j->engine = std::make_unique<forum::Engine>(*store_, *j->net, g.forum_id());
    j->engine->on_post = [this](const forum::Post& p, const std::string& id) {
        postArrivedSoon(q(id), q(p.kind == forum::Kind::Topic ? id : p.topic_id));
    };
    // No Storage snapshots for a group: they would be public. History comes
    // from the store nodes (sealed) and from members, as sealed bundles.
    joined_.push_back(std::move(j));
    return joined_.back().get();
}

LogosForumBackend::Joined* LogosForumBackend::joinedById(const std::string& forumId) {
    for (auto& j : joined_)
        if (j->group.forum_id() == forumId) return j.get();
    return nullptr;
}

forum::Engine* LogosForumBackend::currentEngine() {
    if (currentForum().isEmpty()) return engine_.get();
    // Never the public engine for a private forum: a post meant for a group
    // must not go out in the clear because the group went away.
    auto* j = joinedById(s(currentForum()));
    return j ? j->engine.get() : nullptr;
}

std::string LogosForumBackend::currentForumId() const {
    return currentForum().isEmpty() ? s(forumName()) : s(currentForum());
}

void LogosForumBackend::routeGroupMessage(const QString& topic, const std::string& wire, bool history) {
    for (auto& j : joined_) {
        if (q(j->net->topic()) != topic) continue;
        // Anything on a group's topic that does not open under its key is
        // dropped before it is parsed: it costs one authenticated decryption.
        const auto plain = j->net->open(wire);
        if (!plain) {
            forumLog("dropped a message on a private forum's topic that did not open");
            return;
        }
        const uint64_t before = j->engine->posts_received();
        j->engine->receive(*plain, nowMs(), history ? forum::Origin::History : forum::Origin::Live);
        const auto added = j->engine->posts_received() - before;
        forumLog(std::string("private forum: ") + (added ? std::to_string(added) + " new post(s)" : "a known or non-post message"));
        return;
    }
    // Another app's traffic on the shared delivery_module.
}

std::optional<std::string> LogosForumBackend::openOwnSealed(const std::string& wire) const {
    for (const auto& j : joined_)
        if (auto p = j->net->open(wire)) return p;
    return std::nullopt;
}

void LogosForumBackend::subscribeGroups() {
    for (auto& j : joined_) {
        const QString topic = q(j->net->topic());
        modules().delivery_module.subscribeAsync(topic, [](LogosResult r) {
            if (!r.success) forumLog("a private forum's topic could not be subscribed yet");
        });
    }
}

void LogosForumBackend::pumpGroups() {
    for (auto& j : joined_) {
        j->engine->tick(nowMs());
        j->engine->pump(nowMs());
    }
}

void LogosForumBackend::catchUpGroups() {
    for (auto& j : joined_) {
        const std::string forumId = j->group.forum_id();
        const std::string topic = j->net->topic();
        // The store nodes' archive of the group's topic: sealed messages,
        // opened here; what does not open is dropped.
        net_->history_async(topic, [this, forumId](std::vector<std::string> wires) {
            auto* g = joinedById(forumId);
            if (!g) return;  // left meanwhile
            int added = 0;
            for (const auto& w : wires)
                if (auto m = g->net->open(w))
                    if (g->engine->receive(*m, nowMs(), forum::Origin::History)) ++added;
            forumLog("private forum catch-up: " + std::to_string(wires.size()) + " messages, " + std::to_string(added) + " new");
            // And ask the members, over Delivery, sealed.
            g->engine->request_history(nowMs(), /*via_delivery=*/true);
        });
    }
}

void LogosForumBackend::publishForums() {
    QJsonArray arr;
    arr.append(QJsonObject{{"id", ""}, {"name", forumName()}, {"private", false}, {"current", currentForum().isEmpty()}});
    for (const auto& j : joined_)
        arr.append(QJsonObject{{"id", q(j->group.forum_id())},
                               {"name", q(j->group.name)},
                               {"private", true},
                               {"current", currentForum() == q(j->group.forum_id())}});
    setForumsJson(dumpJson(QJsonDocument(arr)));
}

QString LogosForumBackend::selectForum(QString id) {
    if (!id.isEmpty() && !joinedById(s(id))) return QStringLiteral("error: no such forum");
    setCurrentForum(id);
    publishForums();
    emit postArrived(QString(), QString());  // the view refreshes the list
    return QString();
}

QString LogosForumBackend::createGroup(QString name) {
    if (!groups_) return QStringLiteral("error: the forum is still starting");
    const auto g = forum::Group::create(s(name));
    if (!g) return QStringLiteral("error: give the forum a name (at most %1 bytes)").arg(forum::kMaxGroupName);
    if (!groups_->save(*g, nowMs())) return QStringLiteral("error: the forum could not be saved on this device");
    attachGroup(*g);
    subscribeGroups();
    forumLog("created a private forum");
    selectForum(q(g->forum_id()));
    return q(g->invite());
}

QString LogosForumBackend::joinGroup(QString invite) {
    if (!groups_) return QStringLiteral("error: the forum is still starting");
    const auto g = forum::Group::from_invite(s(invite));
    if (!g) return QStringLiteral("error: this is not a Logos Forum invite (logosforum://join/…)");
    if (!joinedById(g->forum_id())) {
        if (!groups_->save(*g, nowMs())) return QStringLiteral("error: the forum could not be saved on this device");
        attachGroup(*g);
        subscribeGroups();
        forumLog("joined a private forum");
        QTimer::singleShot(0, this, [this]() { catchUpGroups(); });
    }
    selectForum(q(g->forum_id()));
    return QString();
}

QString LogosForumBackend::leaveGroup(QString id) {
    auto it = std::find_if(joined_.begin(), joined_.end(), [&](const auto& j) { return q(j->group.forum_id()) == id; });
    if (it == joined_.end()) return QStringLiteral("error: no such forum");
    modules().delivery_module.unsubscribeAsync(q((*it)->net->topic()), [](LogosResult) {});
    groups_->remove(s(id));
    sodium_memzero((*it)->group.secret.data(), (*it)->group.secret.size());
    joined_.erase(it);
    forumLog("left a private forum");
    if (currentForum() == id) setCurrentForum(QString());
    publishForums();
    publishOutbox();
    emit postArrived(QString(), QString());
    return QString();
}

QString LogosForumBackend::groupInvite(QString id) {
    auto* j = joinedById(s(id));
    if (!j) return QStringLiteral("error: no such forum");
    return q(j->group.invite());
}

QString LogosForumBackend::muteAuthor(QString key) {
    forum::PublicKey k{};
    if (!moderation_ || !parseKey(key, k)) return QStringLiteral("error: not a key");
    for (const auto& a : accounts_)
        if (a.key.pk == k) return QStringLiteral("error: that is one of your accounts");
    moderation_->mute(s(selectedLabel_), k, nowMs());
    emit postArrived(QString(), QString());
    return QString();
}

QString LogosForumBackend::unmuteAuthor(QString key) {
    forum::PublicKey k{};
    if (!moderation_ || !parseKey(key, k)) return QStringLiteral("error: not a key");
    moderation_->unmute(s(selectedLabel_), k);
    emit postArrived(QString(), QString());
    return QString();
}

QString LogosForumBackend::hidePost(QString id) {
    if (!moderation_ || !moderation_->hide(s(selectedLabel_), s(id), nowMs()))
        return moderation_ && moderation_->is_hidden(s(selectedLabel_), s(id)) ? QString() : QStringLiteral("error: not a post");
    emit postArrived(QString(), QString());
    return QString();
}

QString LogosForumBackend::unhidePost(QString id) {
    if (!moderation_) return QStringLiteral("error: the forum is still starting");
    moderation_->unhide(s(selectedLabel_), s(id));
    emit postArrived(QString(), QString());
    return QString();
}

QString LogosForumBackend::moderationList() {
    if (!moderation_) return QStringLiteral("{}");
    QJsonArray muted, hidden;
    for (const auto& m : moderation_->muted(s(selectedLabel_)))
        muted.append(QJsonObject{{"key", q(forum::to_hex(m.key.data(), m.key.size()))}, {"at", static_cast<double>(m.at_ms)}});
    for (const auto& h : moderation_->hidden(s(selectedLabel_))) {
        QString title;
        if (const auto p = store_->get(h.id))
            title = q(p->kind == forum::Kind::Topic ? p->title : p->body.substr(0, forum::utf8_cut(p->body, 0, 80)));
        hidden.append(QJsonObject{{"id", q(h.id)}, {"at", static_cast<double>(h.at_ms)}, {"title", title}});
    }
    return dumpJson(QJsonDocument(QJsonObject{{"muted", muted}, {"hidden", hidden}}));
}
