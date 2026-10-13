// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Accounts beyond the basics: keys sealed with a passphrase, recovery
// phrases, and the inbox (replies to you, your posts). The rules live in
// core/ (keystore.h, recovery.h, inbox.h) and are tested there; this file only
// connects them to the view.
#include "logos_forum_backend.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <sodium.h>

#include <map>

#include "forum/inbox.h"
#include "forum/recovery.h"

using forum::Account;
using forum::Post;

// From logos_forum_backend.cpp.
QJsonObject forumPostJson(const Post& p, const std::string& id, const std::vector<Account>& mine,
                          const QHash<QString, int>& outbox);
void forumLog(const std::string& m);

namespace {

QString q(const std::string& v) { return QString::fromStdString(v); }
std::string s(const QString& v) { return v.toStdString(); }
uint64_t now_ms() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }
QString dump(const QJsonDocument& d) { return QString::fromUtf8(d.toJson(QJsonDocument::Compact)); }

// A passphrase or a recovery phrase from the view: copied once into a string
// that is wiped when it goes away, and the view's copy wiped right away. (The
// copies Qt Remote Objects made on the way here are beyond reach.)
struct Secret {
    std::string v;
    explicit Secret(QString& from) {
        QByteArray b = from.toUtf8();
        v.assign(b.constData(), static_cast<size_t>(b.size()));
        sodium_memzero(b.data(), static_cast<size_t>(b.size()));
        if (!from.isEmpty()) sodium_memzero(from.data(), static_cast<size_t>(from.size()) * sizeof(QChar));
    }
    ~Secret() { sodium_memzero(v.data(), v.size()); }
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
};

// Long enough that Argon2id has something to protect.
constexpr size_t kMinPassphrase = 8;

const char* why_name(forum::Why w) {
    switch (w) {
    case forum::Why::YourTopic: return "topic";
    case forum::Why::AfterYourReply: return "after";
    case forum::Why::Mention: return "mention";
    }
    return "topic";
}

} // namespace

std::vector<Account> LogosForumBackend::selectedView() {
    // Only the selected account's key and label: another account's posts are
    // not "mine" while this one is in use, and no secret is copied.
    std::vector<Account> v;
    for (const auto& a : accounts_)
        if (q(a.label) == selectedLabel_) {
            Account x;
            x.label = a.label;
            x.key.pk = a.key.pk;
            v.push_back(std::move(x));
        }
    return v;
}

QString LogosForumBackend::keysLocked() const {
    if (keys_ && keys_->is_protected() && !keys_->is_unlocked())
        return QStringLiteral("your keys are locked: unlock them with your passphrase to post as an account "
                              "(anonymous posts need no key)");
    for (const auto& a : accounts_)
        if (q(a.label) == selectedLabel_ && sodium_is_zero(a.key.sk.data(), a.key.sk.size()))
            return QStringLiteral("this account's key could not be opened");
    return QString();
}

void LogosForumBackend::reloadAccounts() {
    accounts_ = keys_->accounts();
    if (!selected() && !accounts_.empty()) selectedLabel_ = q(accounts_.front().label);
    publishKeys();
    publishAccounts();
}

void LogosForumBackend::publishKeys() {
    setKeysJson(dump(QJsonDocument(QJsonObject{{"protected", keys_ && keys_->is_protected()},
                                               {"unlocked", keys_ && keys_->is_unlocked()}})));
}

void LogosForumBackend::publishInbox() {
    const Account* sel = store_ ? selected() : nullptr;
    if (!sel) {
        setInboxUnread(0);
        return;
    }
    const auto v = forum::inbox(*store_, s(forumName()), sel->key.pk, store_->inbox_read(sel->label), anonThisSession_);
    setInboxUnread(static_cast<int>(v.unread));
}

// ─── Keys at rest ───────────────────────────────────────────────────────────

QString LogosForumBackend::unlock(QString passphrase) {
    Secret p(passphrase);
    if (!keys_) return QStringLiteral("the forum is still starting");
    switch (keys_->unlock(p.v)) {
    case forum::Unlock::Ok: break;
    case forum::Unlock::WrongPassphrase: return QStringLiteral("wrong passphrase");
    case forum::Unlock::NotProtected: return QStringLiteral("there is no passphrase");
    case forum::Unlock::Damaged: return QStringLiteral("the key settings in forum.db are damaged");
    }
    reloadAccounts();
    forumLog("account keys unlocked");
    return QString();
}

QString LogosForumBackend::lockKeys() {
    if (!keys_ || !keys_->is_protected()) return QStringLiteral("set a passphrase first");
    keys_->lock();
    reloadAccounts();   // the keys in memory are dropped, and wiped as they go
    forumLog("account keys locked");
    return QString();
}

QString LogosForumBackend::setPassphrase(QString current, QString next) {
    Secret c(current), n(next);
    if (!keys_) return QStringLiteral("the forum is still starting");
    if (!n.v.empty() && n.v.size() < kMinPassphrase)
        return QStringLiteral("use at least %1 characters").arg(kMinPassphrase);
    if (!keys_->is_protected()) {
        if (n.v.empty()) return QStringLiteral("choose a passphrase");
        if (!keys_->protect(n.v)) return QStringLiteral("the keys could not be sealed; nothing was changed");
        forumLog("account keys sealed with a passphrase");
    } else {
        if (!keys_->check(c.v)) return QStringLiteral("wrong passphrase");
        if (n.v.empty()) {
            if (!keys_->unprotect(c.v)) return QStringLiteral("the passphrase could not be removed; nothing was changed");
            forumLog("account keys no longer sealed");
        } else {
            if (!keys_->is_unlocked() && keys_->unlock(c.v) != forum::Unlock::Ok) return QStringLiteral("wrong passphrase");
            if (!keys_->protect(n.v)) return QStringLiteral("the keys could not be sealed; nothing was changed");
            forumLog("account passphrase changed");
        }
    }
    reloadAccounts();
    return QString();
}

// ─── Backup ─────────────────────────────────────────────────────────────────

QString LogosForumBackend::recoveryPhrase(QString label, QString passphrase) {
    Secret p(passphrase);
    if (!keys_) return QStringLiteral("error: the forum is still starting");
    if (keys_->is_protected() && !keys_->check(p.v)) return QStringLiteral("error: wrong passphrase");
    for (auto& a : accounts_) {
        if (q(a.label) != label) continue;
        if (sodium_is_zero(a.key.sk.data(), a.key.sk.size())) return QStringLiteral("error: unlock your keys first");
        std::string words = forum::recovery_phrase(a.key.sk);
        const QString out = q(words);
        sodium_memzero(words.data(), words.size());
        a.backed_up = true;
        keys_->save(a, q(a.label) == selectedLabel_);
        publishAccounts();
        forumLog("showed an account's recovery phrase");
        return out;
    }
    return QStringLiteral("error: no such account");
}

QString LogosForumBackend::restoreAccount(QString label, QString words) {
    Secret w(words);
    if (!keys_) return QStringLiteral("the forum is still starting");
    if (keys_->is_protected() && !keys_->is_unlocked()) return QStringLiteral("unlock your keys first");
    std::string why;
    auto key = forum::restore_from_phrase(w.v, &why);
    if (!key) return q(why);
    for (const auto& a : accounts_)
        if (a.key.pk == key->pk) return QStringLiteral("this key is already here, as \"%1\"").arg(q(a.label));
    label = label.trimmed();
    if (label.isEmpty())
        for (int n = 1;; ++n) {
            label = n == 1 ? QStringLiteral("Restored") : QStringLiteral("Restored %1").arg(n);
            bool taken = false;
            for (const auto& a : accounts_) taken = taken || q(a.label) == label;
            if (!taken) break;
        }
    for (const auto& a : accounts_)
        if (q(a.label) == label) return QStringLiteral("an account called \"%1\" already exists").arg(label);
    // Restored as it was written down: backed up, and with no anonymous key,
    // retired key or post history attached (none was ever derived from it).
    Account a{s(label), *key, now_ms(), 0, true};
    if (!keys_->save(a, true)) return QStringLiteral("the account could not be saved");
    accounts_.push_back(a);
    selectedLabel_ = label;
    publishAccounts();
    forumLog("restored an account from its recovery phrase");
    return QString();
}

// ─── Replies to you, your posts ─────────────────────────────────────────────

QString LogosForumBackend::inbox() {
    const Account* sel = store_ ? selected() : nullptr;
    if (!sel) return QStringLiteral("{\"items\":[],\"unread\":0}");
    const auto me = selectedView();
    const auto v = forum::inbox(*store_, s(forumName()), sel->key.pk, store_->inbox_read(sel->label), anonThisSession_);
    std::map<std::string, QString> titles;
    QJsonArray items;
    for (const auto& i : v.items) {
        const Post& p = i.post.post;
        const std::string topic = p.kind == forum::Kind::Topic ? i.post.id : p.topic_id;
        if (!titles.count(topic)) {
            const auto t = store_->get(topic);
            titles[topic] = t ? q(t->title) : QString();
        }
        QJsonObject o = forumPostJson(p, i.post.id, me, {});
        o.insert("why", why_name(i.why));
        o.insert("unread", i.unread);
        o.insert("topicId", q(topic));
        o.insert("topicTitle", titles[topic]);
        items.append(o);
    }
    setInboxUnread(static_cast<int>(v.unread));
    return dump(QJsonDocument(QJsonObject{{"handle", q(forum::mention_handle(sel->key.pk))},
                                          {"unread", static_cast<int>(v.unread)},
                                          {"items", items}}));
}

QString LogosForumBackend::myPosts() {
    const Account* sel = store_ ? selected() : nullptr;
    if (!sel) return QStringLiteral("[]");
    QHash<QString, int> outbox;
    for (const auto& o : store_->outbox()) outbox.insert(q(o.id), o.attempts);
    const auto me = selectedView();
    QJsonArray out;
    for (const auto& st : forum::my_posts(*store_, s(forumName()), sel->key.pk)) {
        const std::string topic = st.post.kind == forum::Kind::Topic ? st.id : st.post.topic_id;
        QJsonObject o = forumPostJson(st.post, st.id, me, outbox);
        o.insert("topicId", q(topic));
        if (st.post.kind == forum::Kind::Reply) {
            const auto t = store_->get(topic);
            o.insert("topicTitle", t ? q(t->title) : QString());
        } else {
            o.insert("topicTitle", q(st.post.title));
        }
        out.append(o);
    }
    return dump(QJsonDocument(out));
}

QString LogosForumBackend::markInboxRead() {
    const Account* sel = store_ ? selected() : nullptr;
    if (!sel) return QStringLiteral("no account selected");
    store_->set_inbox_read(sel->label, now_ms());
    publishInbox();
    return QString();
}
