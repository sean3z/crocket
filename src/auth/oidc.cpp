#include "internal.hpp"

#include "crocket/client.hpp"
#include "crocket/json.hpp"

#include <charconv>
#include <chrono>
#include <string>
#include <utility>

namespace crocket::auth::detail {
namespace {

// One document. The refresh thread blocks in sync_wait; the socket work runs
// on the client's event loop.
constexpr auto kTimeout = std::chrono::milliseconds{5000};
constexpr std::size_t kMaxBody = std::size_t{1} << 20;

struct Url {
  bool https = false;
  std::string host;
  std::string port;
  std::string path;
};

std::expected<Url, std::string> parse_url(std::string_view url) {
  Url u;
  if (url.starts_with("https://")) {
    u.https = true;
    u.port = "443";
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    u.port = "80";
    url.remove_prefix(7);
  } else {
    return std::unexpected("URL scheme must be https or http");
  }
  auto slash = url.find('/');
  auto hostport = url.substr(0, slash);
  u.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  if (auto hash = u.path.find('#'); hash != std::string::npos) u.path.resize(hash);
  if (hostport.empty()) return std::unexpected("URL has no host");
  if (hostport.front() == '[') {
    auto end = hostport.find(']');
    if (end == std::string_view::npos) return std::unexpected("bad IPv6 host");
    u.host = std::string(hostport.substr(1, end - 1));
    if (end + 1 < hostport.size()) {
      if (hostport[end + 1] != ':') return std::unexpected("bad host");
      u.port = std::string(hostport.substr(end + 2));
    }
  } else if (auto colon = hostport.rfind(':'); colon != std::string_view::npos && hostport.find(':') == colon) {
    u.host = std::string(hostport.substr(0, colon));
    u.port = std::string(hostport.substr(colon + 1));
  } else {
    u.host = std::string(hostport);
  }
  if (u.host.empty() || u.port.empty()) return std::unexpected("URL has no host");
  return u;
}

std::string origin_of(const Url& u) {
  std::string o = u.https ? "https://" : "http://";
  if (u.host.find(':') != std::string::npos) o += "[" + u.host + "]";
  else o += u.host;
  bool standard = (u.https && u.port == "443") || (!u.https && u.port == "80");
  if (!standard) o += ":" + u.port;
  return o;
}

std::string resolve(const Url& base, std::string_view loc) {
  if (loc.starts_with("https://") || loc.starts_with("http://")) return std::string(loc);
  if (loc.starts_with("/")) return origin_of(base) + std::string(loc);
  return {};
}

std::chrono::seconds max_age_of(std::string_view cache_control) {
  auto at = cache_control.find("max-age");
  if (at == std::string_view::npos) return {};
  auto eq = cache_control.find('=', at);
  if (eq == std::string_view::npos) return {};
  auto num = cache_control.substr(eq + 1);
  while (!num.empty() && num.front() == ' ') num.remove_prefix(1);
  std::uint64_t v = 0;
  auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
  if (ec != std::errc() || v == 0 || v > 86400ull * 30) return {};
  return std::chrono::seconds(v);
}

std::string failure_text(const ApiError& err) {
  if (!err.detail.empty()) return err.detail;
  if (!err.message.empty()) return err.message;
  return std::string(err.code);
}

std::expected<Fetched, std::string> http_get_once(const Url& u, std::string_view ca_file) {
  http::Client client{{
      .base = origin_of(u),
      .name = "jwks",
      .timeout = kTimeout,
      .connect_timeout = kTimeout,
      .pool = 4,
      .retry = retry::none,
      .breaker = breaker::none,
      .propagate_request_id = false,
      .ca_file = std::string(ca_file),
      .max_body = kMaxBody,
  }};
  http::CallOptions call;
  call.headers = {{"accept", "application/json"}, {"user-agent", "crocket-auth"}};
  call.retry = retry::none;
  call.breaker = breaker::none;
  call.timeout = kTimeout;
  std::expected<Response, ApiError> res;
  try {
    res = sync_wait(client.get(u.path, std::move(call)));
  } catch (const std::exception& e) {
    return std::unexpected(e.what());
  }
  if (!res) return std::unexpected(failure_text(res.error()));
  Fetched out;
  out.status = res->status;
  if (auto cc = res->headers.get("cache-control")) out.max_age = max_age_of(*cc);
  if (out.status >= 300 && out.status < 400) {
    auto loc = res->headers.get("location");
    if (!loc || loc->empty()) return std::unexpected("redirect with no Location");
    out.body = std::string(*loc);
  } else {
    out.body = std::move(res->body);
  }
  return out;
}

}  // namespace

std::expected<Fetched, std::string> http_get(std::string_view url, std::string_view ca_file) {
  std::string current(url);
  try {
    for (int hop = 0; hop < 4; ++hop) {
      auto parsed = parse_url(current);
      if (!parsed) return std::unexpected(parsed.error());
      auto res = http_get_once(*parsed, ca_file);
      if (!res) return res;
      if (res->status >= 300 && res->status < 400 && hop < 3) {
        auto next = resolve(*parsed, res->body);
        if (next.empty()) return std::unexpected("redirect with no Location");
        current = std::move(next);
        continue;
      }
      return res;
    }
  } catch (const std::exception& e) {
    return std::unexpected(e.what());
  }
  return std::unexpected("too many redirects");
}

std::expected<std::string, std::string> parse_discovery(std::string_view json, std::string_view expected_issuer) {
  auto doc = json::parse(json);
  if (!doc || !doc->is_object()) return std::unexpected("openid-configuration is not a JSON object");
  auto* iss = doc->find("issuer");
  auto* jwks = doc->find("jwks_uri");
  if (!iss || !iss->is_string()) return std::unexpected("openid-configuration has no issuer");
  if (iss->as_string() != expected_issuer)
    return std::unexpected("openid-configuration issuer does not match");
  if (!jwks || !jwks->is_string() || jwks->as_string().empty())
    return std::unexpected("openid-configuration has no jwks_uri");
  auto uri = jwks->as_string();
  if (!uri.starts_with("https://") && !uri.starts_with("http://"))
    return std::unexpected("jwks_uri is not an http(s) URL");
  return uri;
}

}  // namespace crocket::auth::detail
