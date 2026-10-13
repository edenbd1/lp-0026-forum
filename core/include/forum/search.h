// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Local full-text search over the titles and bodies of stored posts.
//
// Only posts in the store are searched, and the store holds only posts whose
// signature was checked. The index is SQLite FTS5, kept up to date by
// triggers on the posts table (so every way a post is stored indexes it), and
// built once from what is already stored. Where SQLite has no FTS5 the same
// search runs as a plain scan, with the same results for words.
//
// What the user types is never FTS syntax: each word becomes a quoted string
// (quotes inside doubled), the last one a prefix, all of them required, and
// the whole is passed as a bound parameter. So "NEAR(", a lone quote, "*",
// "OR", "-x" or a column filter are searched for as text, never interpreted.
#pragma once

#include "forum/post.h"

#include <functional>
#include <string>
#include <vector>

struct sqlite3;

namespace forum {

class Store;

struct SearchHit {
    std::string id;        // the matching post
    std::string topic_id;  // the thread it belongs to (its own id for a topic)
    Kind kind = Kind::Topic;
    std::string title;     // the topic's title, for a topic
    std::string snippet;   // the text around the match, plain text
    PublicKey author{};
    uint64_t ts_ms = 0;
};

// The FTS5 query for what the user typed: quoted words, the last a prefix.
// Empty when nothing searchable is left. At most kMaxSearchWords words.
std::string fts_query(const std::string& typed);
constexpr size_t kMaxSearchWords = 16;
constexpr size_t kMaxSearchBytes = 256;

class Search {
public:
    explicit Search(Store& store, bool use_fts = true);
    bool fts() const { return fts_; }
    // Best matches first (FTS5's bm25), at most `limit`. `hidden` drops posts
    // the reader does not want to see (muted authors, once they exist), before
    // they count against the limit.
    std::vector<SearchHit> find(const std::string& forum, const std::string& typed, size_t limit = 50,
                                const std::function<bool(const PublicKey&)>& hidden = {}) const;
private:
    sqlite3* db_;
    bool fts_ = false;
};

} // namespace forum
