#pragma once
// Who is asking, and for which host: client address and scheme behind trusted
// proxies (Config::trusted_proxies), and the Host check (Config::allowed_hosts).

#include "crocket/app.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crocket::detail {

/// "10.0.0.0/8", "192.0.2.7", "fd00::/8" or "::1". IPv4 is held as IPv4-mapped IPv6.
std::optional<IpRange> parse_ip_range(std::string_view s);

/// Replaces rq.remote_addr and rq.scheme with what trusted proxies report.
/// rq.remote_addr must hold the socket peer.
void resolve_client(Request& rq, const std::vector<IpRange>& trusted, ProxyHeader header);

/// Empty if `pattern` is a valid allowed_hosts entry, otherwise why not.
std::string host_pattern_problem(std::string_view pattern);

/// Whether the request's Host (or :authority) matches one of `patterns`.
bool host_allowed(std::string_view host_header, const std::vector<std::string>& patterns, bool dev);

}  // namespace crocket::detail
