// SPDX-License-Identifier: MIT OR Apache-2.0
// The 0.3.6 core (tests/compat/v036, byte for byte as released), driven with
// what this version sends. Plain types only: the two cores never meet in one
// translation unit.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace v036 {

// 0.3.6's decode(): true if it verifies the post; its body and id.
bool decode(const std::string& payload, std::string* body = nullptr, std::string* id = nullptr);

// A fresh 0.3.6 node receives `payloads` in order.
struct Outcome {
    int accepted = 0;                   // receive() returned true
    size_t stored = 0;                  // posts in its store afterwards
    std::vector<std::string> sent;      // what it put on the topic in reply
    std::vector<std::string> bodies;    // the bodies it stored, oldest first
    std::vector<std::string> relayed;   // its snapshot's posts, as it would pass them on
};
Outcome receive(const std::vector<std::string>& payloads, uint64_t now_ms);

} // namespace v036
