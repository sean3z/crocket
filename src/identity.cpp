#include "identity.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

namespace crocket::detail {
namespace {

using Bytes = std::array<std::uint8_t, 16>;

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

/// A bare IPv4 or IPv6 address, IPv4 mapped into IPv6.
std::optional<Bytes> parse_ip(std::string_view s) {
  if (s.empty() || s.size() >= INET6_ADDRSTRLEN) return std::nullopt;
  char buf[INET6_ADDRSTRLEN];
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  Bytes b{};
  in_addr v4;
  if (inet_pton(AF_INET, buf, &v4) == 1) {
    b[10] = b[11] = 0xFF;
    std::memcpy(b.data() + 12, &v4, 4);
    return b;
  }
  in6_addr v6;
  if (inet_pton(AF_INET6, buf, &v6) == 1) {
    std::memcpy(b.data(), &v6, 16);
    return b;
  }
  return std::nullopt;
}

bool is_v4(const Bytes& b) {
  static constexpr std::uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  return std::memcmp(b.data(), prefix, 12) == 0;
}

std::string format_ip(const Bytes& b) {
  char buf[INET6_ADDRSTRLEN];
  if (is_v4(b)) inet_ntop(AF_INET, b.data() + 12, buf, sizeof buf);
  else inet_ntop(AF_INET6, b.data(), buf, sizeof buf);
  return buf;
}

bool contains(const IpRange& r, const Bytes& ip) {
  int bits = r.prefix;
  for (std::size_t i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
    std::uint8_t mask = bits >= 8 ? 0xFF : std::uint8_t(0xFF << (8 - bits));
    if ((r.addr[i] & mask) != (ip[i] & mask)) return false;
  }
  return true;
}

bool trusted_ip(const std::vector<IpRange>& trusted, const Bytes& ip) {
  return std::ranges::any_of(trusted, [&](const IpRange& r) { return contains(r, ip); });
}

/// A node as proxies write it: "192.0.2.7", "192.0.2.7:4711", "2001:db8::1",
/// "[2001:db8::1]" or "[2001:db8::1]:4711". "unknown" and obfuscated names fail.
std::optional<Bytes> parse_node(std::string_view s) {
  s = trim(s);
  if (!s.empty() && s.front() == '[') {
    auto close = s.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    return parse_ip(s.substr(1, close - 1));
  }
  if (auto ip = parse_ip(s)) return ip;
  if (auto colon = s.find(':'); colon != std::string_view::npos && s.find(':', colon + 1) == std::string_view::npos)
    return parse_ip(s.substr(0, colon));  // IPv4 with a port
  return std::nullopt;
}

/// Comma-separated values from every header named `name`, in order.
std::vector<std::string_view> list(const Request& rq, std::string_view name) {
  std::vector<std::string_view> out;
  for (auto& [k, v] : rq.headers) {
    if (!Headers::iequals(k, name)) continue;
    std::string_view rest = v;
    while (true) {
      auto comma = rest.find(',');
      if (auto item = trim(rest.substr(0, comma)); !item.empty()) out.push_back(item);
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return out;
}

std::optional<std::string_view> scheme_of(std::string_view proto) {
  auto p = lower(trim(proto));
  if (p == "https") return "https";
  if (p == "http") return "http";
  return std::nullopt;
}

struct Hop {
  std::string_view node;   // the address the proxy saw
  std::string_view proto;  // the scheme it was reached with ("" if not said)
};

/// RFC 7239: `Forwarded: for=192.0.2.60;proto=https, for="[2001:db8::1]:4711"`.
std::vector<Hop> forwarded_hops(const Request& rq) {
  std::vector<Hop> hops;
  for (auto& [k, v] : rq.headers) {
    if (!Headers::iequals(k, "forwarded")) continue;
    std::string_view rest = v;
    Hop hop;
    bool any = false;
    while (!rest.empty()) {
      // name=value, where value may be a quoted string containing ',' or ';'
      auto eq = rest.find('=');
      if (eq == std::string_view::npos) break;
      auto name = lower(trim(rest.substr(0, eq)));
      rest.remove_prefix(eq + 1);
      rest = trim(rest);
      std::string_view value;
      if (!rest.empty() && rest.front() == '"') {
        auto close = rest.find('"', 1);
        if (close == std::string_view::npos) break;
        value = rest.substr(1, close - 1);
        rest.remove_prefix(close + 1);
      } else {
        auto end = rest.find_first_of(";,");
        value = trim(rest.substr(0, end));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
      }
      if (name == "for") hop.node = value, any = true;
      else if (name == "proto") hop.proto = value, any = true;
      rest = trim(rest);
      if (!rest.empty() && rest.front() == ';') {
        rest.remove_prefix(1);
      } else if (!rest.empty() && rest.front() == ',') {
        rest.remove_prefix(1);
        if (any) hops.push_back(hop);
        hop = {};
        any = false;
      }
    }
    if (any) hops.push_back(hop);
  }
  return hops;
}

std::vector<Hop> x_forwarded_hops(const Request& rq) {
  auto nodes = list(rq, "x-forwarded-for");
  auto protos = list(rq, "x-forwarded-proto");
  std::vector<Hop> hops;
  if (nodes.empty() && !protos.empty()) hops.push_back({"", protos.back()});  // a proxy that only says the scheme
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    // One proto per hop when the proxies append; otherwise the last one set it.
    std::string_view proto = protos.size() == nodes.size() ? protos[i] : protos.empty() ? "" : protos.back();
    hops.push_back({nodes[i], proto});
  }
  return hops;
}

}  // namespace

std::optional<IpRange> parse_ip_range(std::string_view s) {
  auto slash = s.find('/');
  auto ip = parse_ip(s.substr(0, slash));
  if (!ip) return std::nullopt;
  int max = is_v4(*ip) ? 32 : 128;
  int prefix = max;
  if (slash != std::string_view::npos) {
    auto bits = s.substr(slash + 1);
    if (bits.empty() || bits.size() > 3 || !std::ranges::all_of(bits, [](char c) { return c >= '0' && c <= '9'; }))
      return std::nullopt;
    prefix = std::stoi(std::string(bits));
    if (prefix > max) return std::nullopt;
  }
  return IpRange{*ip, is_v4(*ip) ? prefix + 96 : prefix};
}

void resolve_client(Request& rq, const std::vector<IpRange>& trusted, ProxyHeader header) {
  auto peer = parse_ip(rq.remote_addr);
  if (!peer || !trusted_ip(trusted, *peer)) return;  // headers from anyone else are ignored
  auto hops = header == ProxyHeader::Forwarded ? forwarded_hops(rq) : x_forwarded_hops(rq);
  // Walk back from the nearest proxy: each trusted hop vouches for the one before
  // it. The client is the first address no trusted proxy is behind.
  for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
    if (auto s = scheme_of(it->proto)) rq.scheme = *s;  // said by a trusted proxy about its client
    auto ip = parse_node(it->node);
    if (!ip) break;  // "unknown", absent or garbage: keep the last address we can vouch for
    rq.remote_addr = format_ip(*ip);
    if (!trusted_ip(trusted, *ip)) break;
  }
}

std::string host_pattern_problem(std::string_view p) {
  if (p == "*") return {};
  std::string_view name = p.starts_with("*.") ? p.substr(2) : p;
  if (name.starts_with('[') && name.ends_with(']')) name = name.substr(1, name.size() - 2);
  if (parse_ip(name)) return {};
  if (name.empty() || name.find("://") != std::string_view::npos)
    return "is not a host name (write \"api.example.com\" or \"*.example.com\")";
  if (name.find(':') != std::string_view::npos) return "has a port; ports are not checked, so leave it out";
  bool ok = std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
  if (!ok || name.front() == '.' || name.back() == '.' || name.find('*') != std::string_view::npos)
    return "is not a host name (write \"api.example.com\" or \"*.example.com\")";
  return {};
}

bool host_allowed(std::string_view host_header, const std::vector<std::string>& patterns, bool dev) {
  if (patterns.empty()) return true;
  // Drop the port, and the brackets of an IPv6 literal.
  std::string_view h = trim(host_header);
  if (h.starts_with('[')) {
    auto close = h.find(']');
    if (close == std::string_view::npos) return false;
    h = h.substr(1, close - 1);
  } else if (auto colon = h.rfind(':'); colon != std::string_view::npos) {
    h = h.substr(0, colon);
  }
  if (h.ends_with('.')) h.remove_suffix(1);
  if (h.empty()) return false;
  std::string host = lower(h);
  if (dev && (host == "localhost" || host == "127.0.0.1" || host == "::1")) return true;
  for (auto& raw : patterns) {
    std::string p = lower(raw);
    if (p.starts_with('[') && p.ends_with(']')) p = p.substr(1, p.size() - 2);
    if (p == "*" || p == host) return true;
    if (p.starts_with("*.") && host.size() > p.size() - 1 && host.ends_with(std::string_view(p).substr(1))) return true;
  }
  return false;
}

}  // namespace crocket::detail
