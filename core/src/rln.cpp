// SPDX-License-Identifier: MIT OR Apache-2.0
#include "forum/rln.h"

#include <cctype>

namespace forum {

namespace {

// Reads `lit` at `at`, moving past it.
bool lit(const std::string& s, size_t& at, const char* lit) {
    const std::string l(lit);
    if (s.compare(at, l.size(), l) != 0) return false;
    at += l.size();
    return true;
}

// Reads one or more decimal digits at `at`.
bool digits(const std::string& s, size_t& at, std::string& out) {
    const size_t from = at;
    while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) ++at;
    out = s.substr(from, at - from);
    return at > from;
}

} // namespace

std::optional<FundingDetail> parse_funding_detail(const std::string& s) {
    FundingDetail d;
    size_t at = 0;
    while (at < s.size() && at < 64 && std::isxdigit(static_cast<unsigned char>(s[at]))) ++at;
    if (at != 64) return std::nullopt;
    d.payer = s.substr(0, 64);
    if (!lit(s, at, " needs ") || !digits(s, at, d.needs) || !lit(s, at, " native (") ||
        !digits(s, at, d.price) || !lit(s, at, " price + ") || !digits(s, at, d.reserve) ||
        !lit(s, at, " fee reserve)"))
        return std::nullopt;
    // The balance, when given; anything after the shape is ignored, as before.
    size_t rest = at;
    if (lit(s, rest, "; it holds ")) digits(s, rest, d.holds);
    return d;
}

} // namespace forum
