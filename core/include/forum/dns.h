// SPDX-License-Identifier: MIT OR Apache-2.0
// Which DNS servers Logos Delivery asks for the fleet's /dns4/ addresses.
//
// Left to itself, Delivery asks 1.1.1.1 and 1.0.0.1. Networks that block
// outbound DNS to public resolvers (a firewall, a captive office network)
// answer only through their own server, so a node there resolves no entry
// node and finds no peers. The forum hands Delivery the servers the operating
// system uses, then Delivery's own two as a fallback. These are the parsers
// and the rules for that list; reading the system is platform code in the
// plugin.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace forum {

// Delivery's built-in name servers, appended after the system's.
extern const std::vector<std::string> kFallbackDnsServers;
// At most this many servers come from the system or an override.
constexpr size_t kMaxSystemDnsServers = 4;

// A usable name server address in canonical form: a strict dotted-decimal
// IPv4 or an IPv6 literal. Refuses names, ports, zone ids (fe80::1%en0),
// unspecified, multicast, IPv6 link-local and site-local (Windows' fec0::
// placeholders), and IPv4-mapped forms.
std::optional<std::string> dns_server_literal(const std::string& s);

// "nameserver" lines of a resolv.conf, in order.
std::vector<std::string> parse_resolv_conf(const std::string& text);

// Name servers of the default resolvers in `scutil --dns` output (macOS):
// resolvers with no "domain" (those answer only for one domain, like mDNS's
// "local"), the main section first, then the scoped one. In order.
std::vector<std::string> parse_scutil_dns(const std::string& text);

// A comma- or space-separated list (LOGOS_FORUM_DNS, settings.json "dns").
std::vector<std::string> split_dns_list(const std::string& text);

// The list handed to Delivery: valid literals of `found` in order, deduped,
// capped at kMaxSystemDnsServers, then the fallbacks. Empty when `found` has
// no usable server, so the caller leaves Delivery's defaults alone.
std::vector<std::string> dns_servers_for_delivery(const std::vector<std::string>& found);

}  // namespace forum
