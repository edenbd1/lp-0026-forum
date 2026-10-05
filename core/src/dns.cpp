// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/dns.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace forum {

const std::vector<std::string> kFallbackDnsServers{"1.1.1.1", "1.0.0.1"};

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

std::optional<std::string> ipv4(const std::string& s) {
    // Strict dotted decimal: no leading zeros (octal), signs or spaces.
    unsigned v[4];
    size_t i = 0;
    for (int k = 0; k < 4; ++k) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
        if (s[i] == '0' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) return std::nullopt;
        unsigned x = 0;
        size_t d = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            x = x * 10 + static_cast<unsigned>(s[i++] - '0');
            if (++d > 3) return std::nullopt;
        }
        if (x > 255) return std::nullopt;
        v[k] = x;
        if (k < 3) {
            if (i >= s.size() || s[i] != '.') return std::nullopt;
            ++i;
        }
    }
    if (i != s.size()) return std::nullopt;
    if (v[0] == 0 || v[0] >= 224) return std::nullopt;  // this network, multicast, reserved, broadcast
    return s;
}

std::optional<std::string> ipv6(const std::string& s) {
    if (s.find(':') == std::string::npos || s.find('%') != std::string::npos) return std::nullopt;
    unsigned char b[16];
    if (inet_pton(AF_INET6, s.c_str(), b) != 1) return std::nullopt;
    bool zero10 = true;
    for (int k = 0; k < 10; ++k) zero10 = zero10 && b[k] == 0;
    bool zero15 = zero10;
    for (int k = 10; k < 15; ++k) zero15 = zero15 && b[k] == 0;
    if (zero15 && b[15] == 0) return std::nullopt;                       // ::
    if (zero10 && !(zero15 && b[15] == 1)) return std::nullopt;          // v4-mapped and v4-compatible forms, but not ::1
    if (b[0] == 0xff) return std::nullopt;                               // multicast
    if (b[0] == 0xfe && (b[1] & 0x80) == 0x80) return std::nullopt;      // fe80::/10 link-local (needs a zone), fec0::/10 site-local
    char out[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, b, out, sizeof out)) return std::nullopt;
    return std::string(out);
}

}  // namespace

std::optional<std::string> dns_server_literal(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty() || s.size() > 45) return std::nullopt;
    if (auto v4 = ipv4(s)) return v4;
    return ipv6(s);
}

std::vector<std::string> parse_resolv_conf(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& line : lines_of(text)) {
        std::istringstream words(line);
        std::string key, value;
        if (!(words >> key) || key != "nameserver" || !(words >> value)) continue;
        out.push_back(value);
    }
    return out;
}

std::vector<std::string> parse_scutil_dns(const std::string& text) {
    // Each resolver block: "resolver #N", then "  key : value" lines. The
    // scoped section repeats the per-interface resolvers after its header.
    std::vector<std::string> main, scoped, block;
    bool inScoped = false, hasDomain = false, open = false;
    auto close = [&]() {
        if (open && !hasDomain) {
            auto& into = inScoped ? scoped : main;
            into.insert(into.end(), block.begin(), block.end());
        }
        block.clear();
        hasDomain = false;
        open = false;
    };
    for (const auto& raw : lines_of(text)) {
        const std::string line = trim(raw);
        if (line.rfind("DNS configuration", 0) == 0) {
            close();
            inScoped = line.find("scoped") != std::string::npos;
            continue;
        }
        if (line.rfind("resolver #", 0) == 0) {
            close();
            open = true;
            continue;
        }
        if (!open) continue;
        const size_t colon = line.find(" : ");
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon)), value = trim(line.substr(colon + 3));
        if (key == "domain") hasDomain = true;
        else if (key.rfind("nameserver[", 0) == 0) block.push_back(value);
    }
    close();
    main.insert(main.end(), scoped.begin(), scoped.end());
    return main;
}

std::vector<std::string> split_dns_list(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : text) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> dns_servers_for_delivery(const std::vector<std::string>& found) {
    std::vector<std::string> out;
    for (const auto& f : found) {
        const auto lit = dns_server_literal(f);
        if (!lit || std::find(out.begin(), out.end(), *lit) != out.end()) continue;
        out.push_back(*lit);
        if (out.size() == kMaxSystemDnsServers) break;
    }
    if (out.empty()) return out;
    for (const auto& f : kFallbackDnsServers)
        if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    return out;
}

}  // namespace forum
