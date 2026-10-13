// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The Basecamp side of images, reactions and search: it moves image bytes
// between the image pipeline, Logos Storage and the view, and turns the
// core's reactions and search into what the view shows. The rules (what a
// manifest may hold, which bytes may be shown, what a reaction or a request
// costs) live in core/ and src/image_pipeline.cpp, and are tested there.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "forum/media.h"
#include "forum/post.h"
#include "forum/search.h"

class LogosForumBackend;

class ForumMedia {
public:
    explicit ForumMedia(LogosForumBackend& backend);
    ~ForumMedia();

    // Once the engine exists: connects the core's callbacks and Storage's events.
    void start(bool viaMix);

    QString attach(const QString& fileUrl);
    QString discard(const QString& token);
    // The manifest for `tokens`, or "error: …". After the post is stored,
    // published() lets Storage announce them.
    QString manifest(const QString& tokens, std::string* out);
    void published(const QString& tokens);

    QString load(const QString& postId, int index);
    QString full(const QString& postId, int index);
    QString react(const QString& postId, int emoji);
    QString search(const QString& query);

    // What a post shows of its images and reactions (thread view), or only
    // how many images it has (topic list).
    void decorate(QJsonObject& o, const forum::Post& p, const std::string& id, bool full);

    // Storage events for downloads this class started; true if it was one.
    bool downloadProgress(const QString& sessionId, const QString& payload);
    bool downloadDone(const QString& sessionId, bool success, const QString& error);

private:
    struct Pending {
        QByteArray full, thumb;
        QString type, path, session, cid, error;
        int w = 0, h = 0;
        bool done = false;
    };
    struct Download {
        std::string postId, cid, sha256, path;
        int index = 0;
        uint64_t bytes = 0;
    };
    std::vector<forum::ImageRef> images(const std::string& postId) const;
    QString cachePath(const std::string& sha256) const;
    QByteArray cached(const forum::ImageRef& ref) const;
    void startDownload(const std::string& postId, int index, const forum::ImageRef& ref);
    void finish(const QString& session, bool ok, const QString& why);
    QString thumbUrl(const std::string& thumb);

    LogosForumBackend& b_;
    bool viaMix_ = false;
    QString dir_;
    std::map<QString, Pending> pending_;          // token -> attached image
    std::map<QString, Download> downloads_;       // storage session (= CID) -> download
    std::map<std::string, std::pair<std::string, int>> wanted_;  // cid -> the image a reader asked for
    QHash<QString, QString> thumbs_;              // sha256(thumb) -> data URL
    std::unique_ptr<forum::Search> search_;
};
