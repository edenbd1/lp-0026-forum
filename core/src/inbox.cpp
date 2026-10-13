// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/inbox.h"

#include <map>

namespace forum {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Every post of `me` there is to know about: generous, the inbox needs all
// topics and first replies of the key, not only the newest.
constexpr size_t kOwnPostsScanned = 100000;

} // namespace

std::string mention_handle(const PublicKey& pk) { return to_hex(pk.data(), kMentionDigits / 2); }

bool mentions(const std::string& text, const PublicKey& pk) {
    const std::string hex = to_hex(pk.data(), pk.size());
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1)) {
        size_t n = 0;
        bool same = true;
        while (at + 1 + n < text.size() && hex_value(text[at + 1 + n]) >= 0) {
            if (n < hex.size()) same = same && hex_value(text[at + 1 + n]) == hex_value(hex[n]);
            ++n;
        }
        if (same && n >= kMentionDigits && n <= hex.size()) return true;
    }
    return false;
}

InboxView inbox(const Store& store, const std::string& forum, const PublicKey& me, uint64_t read_upto,
                const std::set<std::string>& skip, size_t limit) {
    std::set<std::string> my_topics;
    std::map<std::string, uint64_t> first_reply;  // topic id -> date of my first reply in it
    for (const auto& s : store.by_author(forum, me, kOwnPostsScanned)) {
        if (s.post.kind == Kind::Topic) my_topics.insert(s.id);
        else {
            auto it = first_reply.find(s.post.topic_id);
            if (it == first_reply.end() || s.post.ts_ms < it->second) first_reply[s.post.topic_id] = s.post.ts_ms;
        }
    }
    InboxView v;
    for (auto& s : store.inbox_candidates(forum, me, "@" + mention_handle(me), limit)) {
        if (s.post.author == me || skip.count(s.id)) continue;
        InboxItem item;
        const bool reply = s.post.kind == Kind::Reply;
        const auto fr = reply ? first_reply.find(s.post.topic_id) : first_reply.end();
        if (reply && my_topics.count(s.post.topic_id)) item.why = Why::YourTopic;
        else if (mentions(s.post.title + "\n" + s.post.body, me)) item.why = Why::Mention;
        else if (fr != first_reply.end() && s.post.ts_ms > fr->second) item.why = Why::AfterYourReply;
        else continue;  // "@" and the digits, but not a mention by the rules above
        item.unread = s.received_ms > read_upto;
        if (item.unread) ++v.unread;
        item.post = std::move(s);
        v.items.push_back(std::move(item));
    }
    return v;
}

std::vector<Stored> my_posts(const Store& store, const std::string& forum, const PublicKey& me, size_t limit) {
    return store.by_author(forum, me, limit);
}

} // namespace forum
