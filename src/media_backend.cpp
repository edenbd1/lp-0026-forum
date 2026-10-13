// SPDX-License-Identifier: MIT OR Apache-2.0
#include "media_backend.h"

#include "image_pipeline.h"
#include "logos_forum_backend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#include <iostream>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include "forum/reaction.h"
#include "logos_sdk.h"
#include "logos_types.h"

using forum::ImageRef;

namespace {

constexpr int kChunk = 64 * 1024;
constexpr int kDownloadWatchdogMs = 120000;
constexpr size_t kMaxDownloads = 2;

QString q(const std::string& s) { return QString::fromStdString(s); }
std::string s(const QString& v) { return v.toStdString(); }
uint64_t now_ms() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }

// Same file as the backend's log. Never a CID or post id of ours: a log
// attached to a bug report must not say what this install posted.
void mlog(const QString& dir, const std::string& m) {
    const std::string line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString() + " " + m;
    std::cerr << "[logos_forum] " << line << std::endl;
    QFile f(dir + QStringLiteral("/forum.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) f.write((line + "\n").c_str());
}

bool writePrivate(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return f.write(bytes) == bytes.size();
}

QByteArray readCapped(const QString& path, qint64 cap) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray b = f.read(cap + 1);
    return b.size() > cap ? QByteArray() : b;
}

bool localPeersAllowed() { return qEnvironmentVariableIsSet("LOGOS_FORUM_LOCAL_PEERS"); }

QString dump(const QJsonDocument& d) { return QString::fromUtf8(d.toJson(QJsonDocument::Compact)); }

QStringList tokenList(const QString& tokens) {
    QStringList out;
    for (const auto& v : QJsonDocument::fromJson(tokens.toUtf8()).array())
        if (v.isString() && !out.contains(v.toString())) out << v.toString();
    return out;
}

} // namespace

ForumMedia::ForumMedia(LogosForumBackend& backend) : b_(backend) {}
ForumMedia::~ForumMedia() = default;

void ForumMedia::start(bool viaMix) {
    viaMix_ = viaMix;
    dir_ = b_.dataDir();
    for (const char* sub : {"/media", "/media/out", "/media/in", "/media/cache"}) {
        QDir().mkpath(dir_ + QLatin1String(sub));
        QFile::setPermissions(dir_ + QLatin1String(sub), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    // Attachments not posted before the last quit, and half-downloaded files.
    for (const char* sub : {"/media/out", "/media/in"})
        for (const auto& f : QDir(dir_ + QLatin1String(sub)).entryList(QDir::Files)) QFile::remove(dir_ + QLatin1String(sub) + "/" + f);
    search_ = std::make_unique<forum::Search>(*b_.store_);
    mlog(dir_, std::string("search: ") + (search_->fts() ? "SQLite FTS5" : "plain scan (this SQLite has no FTS5)"));

    auto* engine = b_.engine_.get();
    wire(*engine);
    // Only the public forum answers or follows "who has this CID": a
    // group's images are sealed, and fetched through the DHT alone.
    engine->on_image_provider = [this](const std::string& cid, const std::string& peer, const std::vector<std::string>& addrs) {
        // A peer says it has an image we asked for. Over Mix nothing is
        // dialled (that would reveal us); otherwise only public addresses.
        if (viaMix_) return;
        QStringList list;
        for (const auto& a : addrs)
            if (localPeersAllowed() || forum::is_public_multiaddr(a)) list << q(a);
        if (list.isEmpty()) return;
        mlog(dir_, "an image provider answered; dialling it");
        b_.modules().storage_module.connectAsync(q(peer), list, [this, cid](LogosResult r) {
            if (!r.success) return;
            // Connected: if the first attempt already gave up, try once more.
            for (const auto& [session, d] : downloads_)
                if (d.cid == cid) return;  // still running
            const auto w = wanted_.find(cid);
            if (w == wanted_.end() || downloads_.size() >= kMaxDownloads) return;
            const auto [postId, index] = w->second;
            wanted_.erase(w);
            const auto imgs = images(postId);
            if (index >= 0 && index < static_cast<int>(imgs.size()) && cached(imgs[static_cast<size_t>(index)]).isEmpty())
                startDownload(postId, index, imgs[static_cast<size_t>(index)]);
        });
    };
    b_.modules().storage_module.onStorageUploadDone([this](const QString& payload) {
        const auto j = nlohmann::json::parse(s(payload), nullptr, false);
        if (!j.is_object()) return;
        const QString session = q(j.value("sessionId", std::string()));
        for (auto& [token, p] : pending_) {
            if (p.session != session || p.done) continue;
            p.done = true;
            const bool ok = j.value("success", false) && forum::is_valid_cid(j.value("cid", std::string()));
            QFile::remove(p.path);
            if (ok) {
                p.cid = q(j.value("cid", std::string()));
                writePrivate(cachePath(forum::sha256_hex(p.full.toStdString())), p.full);  // ours: shown without fetching
                emit b_.imageUploaded(token, QStringLiteral("ready"), QString());
            } else {
                p.error = q(j.value("error", std::string("upload failed")));
                emit b_.imageUploaded(token, QStringLiteral("failed"), p.error);
            }
            return;
        }
    });
}

void ForumMedia::wire(forum::Engine& engine) {
    engine.on_reaction = [this](const std::string& target) { emit b_.reactionsChanged(q(target)); };
}

forum::Engine* ForumMedia::engineFor(const std::string& forumId) const {
    if (forumId == s(b_.forumName())) return b_.engine_.get();
    auto* j = b_.joinedById(forumId);
    return j ? j->engine.get() : nullptr;
}

std::optional<forum::GroupKey> ForumMedia::groupKey(const std::string& forumId) const {
    if (forumId == s(b_.forumName())) return std::nullopt;
    auto* j = b_.joinedById(forumId);
    if (!j) return std::nullopt;
    return j->group.seal_key();
}

QString ForumMedia::cachePath(const std::string& sha256) const {
    return dir_ + QStringLiteral("/media/cache/") + q(sha256);
}

QByteArray ForumMedia::cached(const ImageRef& ref) const {
    // Checked every time it is read: a file changed on disk is not shown either.
    const QByteArray b = readCapped(cachePath(ref.sha256), static_cast<qint64>(forum::kMaxImageBytes));
    return forum::image_bytes_match(b.toStdString(), ref) ? b : QByteArray();
}

QString ForumMedia::attach(const QString& fileUrl) {
    if (!b_.storageReady_) return QStringLiteral("error: Logos Storage is not ready yet");
    if (pending_.size() >= 16) return QStringLiteral("error: too many images waiting");
    // A local file the user picked, by URL from the file dialog; nothing else.
    const QUrl url(fileUrl);
    if (!url.isLocalFile()) return QStringLiteral("error: not a local file");
    const media::Prepared p = media::prepareFile(url.toLocalFile());
    if (!p.error.isEmpty()) return QStringLiteral("error: this image was refused: %1").arg(p.error);
    uint8_t raw[12];
    randombytes_buf(raw, sizeof raw);
    const QString token = q(forum::to_hex(raw, sizeof raw));
    Pending pend;
    pend.forum = b_.currentForumId();
    const bool group = pend.forum != s(b_.forumName());
    const auto key = groupKey(pend.forum);
    if (group && !key) return QStringLiteral("error: this private forum is no longer joined");
    pend.full = p.full;
    pend.thumb = p.thumb;
    pend.type = p.type;
    pend.w = p.w;
    pend.h = p.h;
    pend.path = dir_ + QStringLiteral("/media/out/") + token + (p.type == QLatin1String("image/png") ? ".png" : ".jpg");
    // In a private group the bytes on Logos Storage are sealed with the
    // group's key: a CID is readable by anyone who learns it.
    const QByteArray stored = key ? QByteArray::fromStdString(forum::seal_image(*key, pend.forum, p.full.toStdString())) : p.full;
    if (stored.isEmpty() || !writePrivate(pend.path, stored)) return QStringLiteral("error: the image could not be written");
    pending_[token] = pend;
    // Not announced (advertise=false) until the post is: an image attached
    // and then dropped is never offered to anyone.
    b_.modules().storage_module.uploadUrlAsync(pend.path, kChunk, false, [this, token](LogosResult r) {
        auto it = pending_.find(token);
        if (it == pending_.end()) return;
        if (!r.success) {
            it->second.done = true;
            it->second.error = r.getError();
            QFile::remove(it->second.path);
            emit b_.imageUploaded(token, QStringLiteral("failed"), it->second.error);
            return;
        }
        it->second.session = r.getString();
    });
    mlog(dir_, "attached an image (" + std::to_string(p.full.size()) + " bytes, re-encoded)");
    return dump(QJsonDocument(QJsonObject{{"token", token},
                                          {"thumb", media::dataUrl(p.thumb, QStringLiteral("image/jpeg"))},
                                          {"w", p.w}, {"h", p.h}, {"bytes", static_cast<double>(p.full.size())},
                                          {"type", p.type}}));
}

QString ForumMedia::discard(const QString& token) {
    auto it = pending_.find(token);
    if (it == pending_.end()) return QString();
    QFile::remove(it->second.path);
    if (!it->second.cid.isEmpty()) {
        QFile::remove(cachePath(forum::sha256_hex(it->second.full.toStdString())));
        b_.modules().storage_module.removeAsync(it->second.cid, [](LogosResult) {});
    }
    pending_.erase(it);
    return QString();
}

QString ForumMedia::manifest(const QString& tokens, std::string* out) {
    const QStringList list = tokenList(tokens);
    if (list.isEmpty()) return QString();
    if (list.size() > static_cast<int>(forum::kMaxImages))
        return QStringLiteral("error: at most %1 images per post").arg(forum::kMaxImages);
    std::vector<ImageRef> refs;
    for (const auto& t : list) {
        const auto it = pending_.find(t);
        if (it == pending_.end()) return QStringLiteral("error: an attached image is gone; attach it again");
        if (it->second.forum != b_.currentForumId())
            return QStringLiteral("error: an image was attached in another forum; attach it again here");
        if (!it->second.done) return QStringLiteral("error: an image is still being stored; try again in a moment");
        if (it->second.cid.isEmpty()) return QStringLiteral("error: an image could not be stored: %1").arg(it->second.error);
        ImageRef r;
        r.cid = s(it->second.cid);
        r.sha256 = forum::sha256_hex(it->second.full.toStdString());
        r.bytes = static_cast<uint64_t>(it->second.full.size());
        r.w = static_cast<uint32_t>(it->second.w);
        r.h = static_cast<uint32_t>(it->second.h);
        r.type = s(it->second.type);
        r.thumb = it->second.thumb.toStdString();
        refs.push_back(std::move(r));
    }
    *out = forum::make_manifest(refs);
    if (out->empty()) return QStringLiteral("error: the images break the forum's limits");
    return QString();
}

void ForumMedia::published(const QString& tokens) {
    for (const auto& t : tokenList(tokens)) {
        auto it = pending_.find(t);
        if (it == pending_.end()) continue;
        // Now announced and served: readers can fetch it.
        b_.modules().storage_module.setAdvertiseAsync(it->second.cid, true, [](LogosResult) {});
        if (auto* e = engineFor(it->second.forum)) e->add_own_image(s(it->second.cid));
        pending_.erase(it);
    }
}

std::vector<ImageRef> ForumMedia::images(const std::string& postId, std::string* forum) const {
    const auto p = b_.store_->get(postId);
    if (!p) return {};
    if (forum) *forum = p->forum;
    const auto imgs = forum::manifest_images(p->body, b_.engine_->manifest(postId));
    return imgs ? *imgs : std::vector<ImageRef>{};
}

QString ForumMedia::thumbUrl(const std::string& thumb) {
    const QString key = q(forum::sha256_hex(thumb));
    const auto it = thumbs_.constFind(key);
    if (it != thumbs_.constEnd()) return it.value();
    // The author's thumbnail bytes are decoded and written again here: the
    // view never decodes what someone else wrote.
    QString mime;
    const QByteArray shown = media::displayCopy(QByteArray::fromStdString(thumb), static_cast<qint64>(forum::kMaxThumbBytes),
                                                static_cast<int>(forum::kMaxThumbSide), &mime);
    const QString url = media::dataUrl(shown, mime);
    if (thumbs_.size() > 512) thumbs_.clear();
    thumbs_.insert(key, url);
    return url;
}

void ForumMedia::decorate(QJsonObject& o, const forum::Post& p, const std::string& id, bool full) {
    const auto line = forum::image_line(p.body);
    if (line) {
        const auto imgs = forum::manifest_images(p.body, b_.engine_->manifest(id));
        o.insert("imageCount", static_cast<int>(line->count));
        if (full) {
            QJsonArray list;
            if (imgs)
                for (const auto& r : *imgs)
                    list.append(QJsonObject{{"w", static_cast<int>(r.w)}, {"h", static_cast<int>(r.h)},
                                            {"bytes", static_cast<double>(r.bytes)}, {"type", q(r.type)},
                                            {"thumb", thumbUrl(r.thumb)},
                                            {"cached", QFileInfo::exists(cachePath(r.sha256))}});
            o.insert("images", list);
            // The signed line names images whose manifest has not reached us.
            o.insert("imagesMissing", imgs ? 0 : static_cast<int>(line->count));
        }
    }
    if (!full) return;
    const auto counts = b_.engine_->reactions().counts(id);
    QJsonArray n, mine;
    const forum::Account* as = b_.selected();
    for (int e = 0; e < forum::kEmojiCount; ++e) {
        n.append(counts[e]);
        const auto r = as ? b_.engine_->reactions().get(as->key.pk, id, e) : std::nullopt;
        mine.append(r && r->on);
    }
    o.insert("reactions", n);
    o.insert("reacted", mine);
}

QString ForumMedia::load(const QString& postId, int index) {
    std::string forumId;
    const auto imgs = images(s(postId), &forumId);
    if (index < 0 || index >= static_cast<int>(imgs.size())) return QStringLiteral("error: no such image");
    forum::Engine* engine = engineFor(forumId);
    if (!engine) return QStringLiteral("error: this private forum is no longer joined");
    const ImageRef& ref = imgs[static_cast<size_t>(index)];
    if (!cached(ref).isEmpty()) {
        emit b_.imageState(postId, index, QStringLiteral("ready"), QString());
        return QString();
    }
    if (!b_.storageReady_) return QStringLiteral("error: Logos Storage is not ready yet");
    for (const auto& [session, d] : downloads_)
        if (d.cid == ref.cid) return QString();  // already on its way
    if (downloads_.size() >= kMaxDownloads) return QStringLiteral("error: two images are loading already; wait a moment");
    startDownload(s(postId), index, ref);
    if (wanted_.size() > 64) wanted_.clear();
    wanted_[ref.cid] = {s(postId), index};
    // And ask the topic who has it, for when the DHT cannot find it (both
    // ends behind NAT). Paced in the engine, once a minute per CID.
    engine->request_image(ref.cid, now_ms());  // sealed, on a group's topic
    return QString();
}

void ForumMedia::startDownload(const std::string& postId, int index, const ImageRef& ref) {
    uint8_t raw[8];
    randombytes_buf(raw, sizeof raw);
    const auto p = b_.store_->get(postId);
    Download d{postId, ref.cid, ref.sha256, s(dir_ + QStringLiteral("/media/in/") + q(forum::to_hex(raw, sizeof raw))),
               p ? p->forum : std::string(), index, ref.bytes};
    const QString session = q(ref.cid);  // storage_module names a download session by its CID
    downloads_[session] = d;
    emit b_.imageState(q(postId), index, QStringLiteral("loading"), QString());
    mlog(dir_, std::string("fetching an image from Logos Storage") + (viaMix_ ? " over Mix" : ""));
    // Not re-served (advertise=false): reading an image does not make this
    // node a provider of it.
    b_.modules().storage_module.downloadToUrlAsync(session, q(d.path), false, kChunk, viaMix_, false,
        [this, session](LogosResult r) {
            if (!r.success) finish(session, false, QStringLiteral("not found on Logos Storage: %1").arg(r.getError()));
        }, Timeout(kDownloadWatchdogMs));
    QTimer::singleShot(kDownloadWatchdogMs, &b_, [this, session]() {
        if (!downloads_.count(session)) return;
        b_.modules().storage_module.downloadCancelAsync(session, [](LogosResult) {});
        finish(session, false, QStringLiteral("timed out: no provider answered"));
    });
}

bool ForumMedia::downloadProgress(const QString& sessionId, const QString& payload) {
    const auto it = downloads_.find(sessionId);
    if (it == downloads_.end()) return false;
    // A dataset larger than the signed size is not the image: stop at once.
    const auto j = nlohmann::json::parse(s(payload), nullptr, false);
    const auto num = [&](const char* k) -> uint64_t {
        if (!j.is_object()) return 0;
        const auto f = j.find(k);
        return f != j.end() && f->is_number_unsigned() ? f->get<uint64_t>() : 0;
    };
    const bool group = groupKey(it->second.forum).has_value();
    const uint64_t most = group ? forum::sealed_image_size(it->second.bytes) : it->second.bytes;
    if (num("total") > most || num("bytes") > most) {
        b_.modules().storage_module.downloadCancelAsync(sessionId, [](LogosResult) {});
        finish(sessionId, false, QStringLiteral("larger than the signed size: not shown"));
    }
    return true;
}

bool ForumMedia::downloadDone(const QString& sessionId, bool success, const QString& error) {
    if (!downloads_.count(sessionId)) return false;
    finish(sessionId, success, error);
    return true;
}

void ForumMedia::finish(const QString& session, bool ok, const QString& why) {
    const auto it = downloads_.find(session);
    if (it == downloads_.end()) return;
    const Download d = it->second;
    downloads_.erase(it);
    const QString path = q(d.path);
    QString state = QStringLiteral("failed"), detail = why;
    if (ok) {
        QByteArray bytes = readCapped(path, static_cast<qint64>(forum::sealed_image_size(forum::kMaxImageBytes)));
        // A group's image is opened with the group's key first; what does not
        // open is not the image.
        if (const auto key = groupKey(d.forum)) {
            const auto plain = forum::open_image(*key, d.forum, bytes.toStdString());
            bytes = plain ? QByteArray::fromStdString(*plain) : QByteArray();
        } else if (d.forum != s(b_.forumName())) {
            bytes.clear();  // a group left meanwhile
        }
        ImageRef ref;
        ref.sha256 = d.sha256;
        ref.bytes = d.bytes;
        // The signed SHA-256, or nothing is shown.
        if (forum::image_bytes_match(bytes.toStdString(), ref) && writePrivate(cachePath(d.sha256), bytes)) {
            state = QStringLiteral("ready");
            detail.clear();
            mlog(dir_, "an image arrived and matches its signed hash (" + std::to_string(bytes.size()) + " bytes)");
        } else {
            detail = QStringLiteral("the bytes received are not the image the author signed: not shown");
            mlog(dir_, "an image did not match its signed hash; discarded");
        }
    } else {
        mlog(dir_, "an image could not be fetched: " + s(why));
    }
    QFile::remove(path);
    emit b_.imageState(q(d.postId), d.index, state, detail);
}

QString ForumMedia::full(const QString& postId, int index) {
    const auto imgs = images(s(postId));
    if (index < 0 || index >= static_cast<int>(imgs.size())) return QString();
    const QByteArray bytes = cached(imgs[static_cast<size_t>(index)]);
    if (bytes.isEmpty()) return QString();
    QString mime;
    return media::dataUrl(media::displayCopy(bytes, static_cast<qint64>(forum::kMaxImageBytes), media::kDisplaySide, &mime), mime);
}

QString ForumMedia::react(const QString& postId, int emoji) {
    if (emoji < 0 || emoji >= forum::kEmojiCount) return QStringLiteral("error: unknown reaction");
    forum::Account* as = b_.selected();
    if (!as) return QStringLiteral("error: no account selected");
    const auto had = b_.engine_->reactions().get(as->key.pk, s(postId), emoji);
    const bool on = !(had && had->on);
    forum::Engine* engine = b_.currentEngine();  // the reaction goes where the post is, sealed in a group
    if (!engine) return QStringLiteral("error: this private forum is no longer joined");
    if (engine->react(*as, s(postId), emoji, on, now_ms()).empty()) return QStringLiteral("error: this post cannot be reacted to");
    QTimer::singleShot(0, &b_, [this]() { b_.pump(); });
    return QString();
}

QString ForumMedia::search(const QString& query) {
    QJsonArray out;
    if (!search_) return dump(QJsonDocument(out));
    // The forum shown (public, or a joined group), without what this
    // account muted or hid (moderation.h): a muted key, a hidden post, or a
    // reply in a hidden thread.
    const std::string forumName = b_.currentForumId();
    const std::string account = s(b_.selectedLabel_);
    forum::Moderation* mod = b_.moderation_.get();
    const auto hits = search_->find(forumName, s(query), 50, [&](const forum::SearchHit& h) {
        return mod && (mod->is_muted(account, h.author) || mod->is_hidden(account, h.id) || mod->is_hidden(account, h.topic_id));
    });
    for (const auto& h : hits) {
        const auto p = b_.store_->get(h.id);
        if (!p || p->ts_ms > now_ms() + forum::Engine::kMaxClockSkewMs) continue;
        QString title = q(h.title);
        if (h.kind == forum::Kind::Reply) {
            const auto t = b_.store_->get(h.topic_id);
            title = t ? q(t->title) : QStringLiteral("(topic not received yet)");
        }
        QString author = p->mode == forum::Mode::Alias ? q(forum::display_alias(p->alias)) : QString();
        bool mine = false;
        for (const auto& a : b_.accounts_)
            if (a.key.pk == p->author) {
                mine = true;
                if (p->mode == forum::Mode::Identity) author = q(a.label);
            }
        out.append(QJsonObject{{"id", q(h.id)}, {"topicId", q(h.topic_id)},
                               {"kind", h.kind == forum::Kind::Topic ? "topic" : "reply"},
                               {"title", title}, {"snippet", q(h.snippet)},
                               {"mode", p->mode == forum::Mode::Identity ? "identity" : p->mode == forum::Mode::Alias ? "alias" : "anonymous"},
                               {"author", author},
                               {"authorKey", q(forum::to_hex(p->author.data(), p->author.size()))},
                               {"mine", mine},
                               {"ts", static_cast<double>(p->ts_ms)}});
    }
    return dump(QJsonDocument(out));
}

// ─── The backend's slots for all of the above ───────────────────────────────

QString LogosForumBackend::attachImage(QString fileUrl) {
    return media_ ? media_->attach(fileUrl) : QStringLiteral("error: the forum is still starting");
}
QString LogosForumBackend::discardImage(QString token) { return media_ ? media_->discard(token) : QString(); }
QString LogosForumBackend::createTopicWithImages(QString title, QString body, int mode, QString alias, QString tokens) {
    return composeWithImages(true, QString(), body, title, mode, alias, tokens);
}
QString LogosForumBackend::replyWithImages(QString topicId, QString body, int mode, QString alias, QString tokens) {
    if (topicId.isEmpty()) return QStringLiteral("error: no topic");
    return composeWithImages(false, topicId, body, QString(), mode, alias, tokens);
}
QString LogosForumBackend::composeWithImages(bool topic, const QString& target, const QString& text, const QString& title,
                                             int mode, const QString& alias, const QString& tokens) {
    if (!media_) return QStringLiteral("error: the forum is still starting");
    if (mode == 2)
        return QStringLiteral("error: images cannot be posted anonymously: your own Storage node serves them, and would link the post to you");
    std::string manifest;
    const QString err = media_->manifest(tokens, &manifest);
    if (!err.isEmpty()) return err;
    const QString r = compose(topic, target, text, title, mode, alias, manifest);
    if (!r.startsWith(QLatin1String("error: "))) media_->published(tokens);
    return r;
}
QString LogosForumBackend::loadImage(QString postId, int index) { return media_ ? media_->load(postId, index) : QString(); }
QString LogosForumBackend::fullImage(QString postId, int index) { return media_ ? media_->full(postId, index) : QString(); }
QString LogosForumBackend::react(QString postId, int emoji) {
    return media_ ? media_->react(postId, emoji) : QStringLiteral("error: the forum is still starting");
}
QString LogosForumBackend::search(QString query) { return media_ ? media_->search(query) : QStringLiteral("[]"); }
