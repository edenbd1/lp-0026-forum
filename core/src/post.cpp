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

namespace {
// Decodes one UTF-8 character at `i`; false (and one byte skipped) if invalid.
bool next_char(const std::string& s, size_t& i, uint32_t& cp) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) { ++i; return false; }
    cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) { ++i; return false; }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    const uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < least[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return false; }
    i += n;
    return true;
}
bool is_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool is_hidden(uint32_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xAD || c == 0x34F || c == 0x61C || c == 0x115F ||
           c == 0x1160 || c == 0x17B4 || c == 0x17B5 || (c >= 0x180B && c <= 0x180F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0xFEFF || c == 0xFFA0 || (c >= 0xFFF0 && c <= 0xFFFB) || (c >= 0xE0000 && c <= 0xE0FFF);
}
} // namespace

std::string display_alias(const std::string& alias) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < alias.size();) {
        const size_t at = i;
        uint32_t c = 0;
        if (!next_char(alias, i, c)) continue;
        if (is_space(c)) { space = !out.empty(); continue; }
        if (is_hidden(c)) continue;
        if (space) out.push_back(' ');
        space = false;
        out.append(alias, at, i - at);
    }
    return out;
}

} // namespace forum
