// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/post.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <stdexcept>

namespace forum {
namespace {

using json = nlohmann::json;

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_str(std::vector<uint8_t>& out, const std::string& s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

const char* kind_str(Kind k) { return k == Kind::Topic ? "topic" : "reply"; }

const char* mode_str(Mode m) {
    switch (m) {
    case Mode::Identity: return "id";
    case Mode::Alias: return "alias";
    case Mode::Anonymous: return "anon";
    }
    return "id";
}

std::string b64(const uint8_t* d, size_t n) {
    std::string out(sodium_base64_encoded_len(n, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), d, n, sodium_base64_VARIANT_ORIGINAL);
    out.resize(out.find('\0'));
    return out;
}

template <size_t N> bool unb64(const std::string& s, std::array<uint8_t, N>& out) {
    size_t len = 0;
    if (sodium_base642bin(out.data(), N, s.data(), s.size(), nullptr, &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        return false;
    return len == N;
}

struct Init {
    Init() {
        if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
    }
};
const Init init_once;

} // namespace

std::vector<uint8_t> Post::signing_bytes() const {
    std::vector<uint8_t> out;
    const std::string domain = "logos-forum:post:v1";
    put_str(out, domain);
    put_u32(out, static_cast<uint32_t>(version));
    put_str(out, kind_str(kind));
    put_str(out, forum);
    put_str(out, topic_id);
    put_str(out, title);
    put_str(out, body);
    put_str(out, mode_str(mode));
    put_str(out, alias);
    put_u64(out, ts_ms);
    out.insert(out.end(), author.begin(), author.end());
    return out;
}

std::string Post::id() const {
    const auto bytes = signing_bytes();
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, bytes.data(), bytes.size());
    return to_hex(h, sizeof h);
}

void sign(Post& p, const SecretKey& sk) {
    const auto bytes = p.signing_bytes();
    crypto_sign_detached(p.sig.data(), nullptr, bytes.data(), bytes.size(), sk.data());
}

Check verify(const Post& p) {
    if (p.version != 1 || p.forum.empty() || p.forum.size() > kMaxForum) return Check::Malformed;
    if (p.title.size() > kMaxTitle || p.body.size() > kMaxBody || p.alias.size() > kMaxAlias)
        return Check::TooLong;
    if (p.kind == Kind::Topic && p.title.empty()) return Check::MissingTitle;
    if (p.kind == Kind::Reply && p.topic_id.size() != 64) return Check::MissingTopic;
    if (p.mode == Mode::Alias && p.alias.empty()) return Check::Malformed;
    if (p.mode != Mode::Alias && !p.alias.empty()) return Check::Malformed;
    const auto bytes = p.signing_bytes();
    if (crypto_sign_verify_detached(p.sig.data(), bytes.data(), bytes.size(), p.author.data()) != 0)
        return Check::BadSignature;
    return Check::Ok;
}

std::string encode(const Post& p) {
    json j = {
        {"v", p.version},       {"kind", kind_str(p.kind)}, {"forum", p.forum},
        {"body", p.body},       {"mode", mode_str(p.mode)}, {"ts", p.ts_ms},
        {"author", b64(p.author.data(), p.author.size())},
        {"sig", b64(p.sig.data(), p.sig.size())},
    };
    if (p.kind == Kind::Topic) j["title"] = p.title;
    if (p.kind == Kind::Reply) j["topic"] = p.topic_id;
    if (p.mode == Mode::Alias) j["alias"] = p.alias;
    return j.dump();
}

std::optional<Post> decode(const std::string& text, Check* why) {
    auto fail = [&](Check c) -> std::optional<Post> {
        if (why) *why = c;
        return std::nullopt;
    };
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail(Check::Malformed);
    try {
        Post p;
        p.version = j.at("v").get<int>();
        const auto kind = j.at("kind").get<std::string>();
        if (kind == "topic") p.kind = Kind::Topic;
        else if (kind == "reply") p.kind = Kind::Reply;
        else return fail(Check::Malformed);
        const auto mode = j.at("mode").get<std::string>();
        if (mode == "id") p.mode = Mode::Identity;
        else if (mode == "alias") p.mode = Mode::Alias;
        else if (mode == "anon") p.mode = Mode::Anonymous;
        else return fail(Check::Malformed);
        p.forum = j.at("forum").get<std::string>();
        p.body = j.at("body").get<std::string>();
        p.ts_ms = j.at("ts").get<uint64_t>();
        p.title = j.value("title", "");
        p.topic_id = j.value("topic", "");
        p.alias = j.value("alias", "");
        if (!unb64(j.at("author").get<std::string>(), p.author)) return fail(Check::Malformed);
        if (!unb64(j.at("sig").get<std::string>(), p.sig)) return fail(Check::Malformed);
        const Check c = verify(p);
        if (c != Check::Ok) return fail(c);
        if (why) *why = Check::Ok;
        return p;
    } catch (const std::exception&) {
        return fail(Check::Malformed);
    }
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(d[data[i] >> 4]);
        s.push_back(d[data[i] & 15]);
    }
    return s;
}

} // namespace forum
