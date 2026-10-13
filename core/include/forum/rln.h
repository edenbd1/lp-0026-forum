// SPDX-License-Identifier: MIT OR Apache-2.0
// What the RLN module's provisioning says while it waits for funds.
//
// The detail string reads "<payer> needs <n> native (<price> price + <reserve>
// fee reserve); it holds <m>", the last clause only once the balance is known.
// It is parsed by hand: a QRegularExpression would JIT-compile inside
// Basecamp's hardened runtime, which has no allow-jit and traps.
#pragma once

#include <optional>
#include <string>

namespace forum {

struct FundingDetail {
    std::string payer;    // 64 hex digits
    std::string needs, price, reserve;
    std::string holds;    // empty when the module did not say
};

// Nothing if the string does not have that shape.
std::optional<FundingDetail> parse_funding_detail(const std::string& detail);

} // namespace forum
