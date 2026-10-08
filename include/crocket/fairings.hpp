#pragma once
// Standard fairings. All are short, synchronous and thread-safe.

#include "crocket/app.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crocket {

/// One line per request, through Config::log (level, sampling, sink), e.g.
///   {"ts":"…","level":"info","msg":"request","request_id":"…","trace_id":"…","method":"GET",
///    "route":"/hello/{name}/{age}","handler":"api::hello","status":200,"code":"","duration_ms":0.41,
///    "bytes_in":0,"bytes_out":27,"proto":"h2","client":"203.0.113.9","user_agent":"curl/8.5.0"}
/// with "subject" once Auth has verified the caller, and ApiError::detail as
/// "detail" (the only place it appears outside the dev profile). In the dev
/// profile the line is readable text instead:
///   14:02:11.504 GET /hello/Ada/400 404 0.21ms api::hello path.invalid: capture '{age}' did not parse [id]
/// Attach it first so on_response (reverse order) sees the final response.
class Logger {
 public:
  void on_ignite(Ignite& ig);
  void on_response(const Request& rq, Response& rs);
  void on_shutdown();

 private:
  std::shared_ptr<log::detail::Hub> hub_;
};

/// CORS as a fairing. Default is deny: no cross-origin headers are ever
/// added and preflights are refused with 403 cors.denied.
class Cors {
 public:
  static Cors deny();
  /// Exact origins ("https://app.example.com"), or "*" for any origin.
  static Cors allow_origins(std::vector<std::string> origins);

  Cors& allow_methods(std::vector<http::Method> methods);
  Cors& allow_headers(std::vector<std::string> headers);
  Cors& expose_headers(std::vector<std::string> headers);
  Cors& allow_credentials(bool yes = true);
  Cors& max_age(std::chrono::seconds age);

  void on_ignite(Ignite& ig);
  std::optional<Response> on_request(Request& rq);
  void on_response(const Request& rq, Response& rs);

 private:
  [[nodiscard]] bool origin_allowed(std::string_view origin) const;
  [[nodiscard]] std::string allow_origin_value(std::string_view origin) const;
  std::vector<std::string> origins_;
  std::vector<http::Method> methods_{http::Method::Get, http::Method::Head, http::Method::Post, http::Method::Put,
                                     http::Method::Patch, http::Method::Delete};
  std::vector<std::string> headers_{"content-type", "authorization", "x-request-id"};
  std::vector<std::string> expose_{"x-request-id"};
  bool credentials_ = false;
  std::chrono::seconds max_age_{600};
};

/// Security headers on every response, like Rocket's Shield. Crocket attaches
/// Shield{} at ignite unless a Shield is already attached; attach
/// Shield::none() to send none of them.
///
///   x-content-type-options: nosniff
///   x-frame-options: DENY
///   content-security-policy: default-src 'none'; frame-ancestors 'none'
///   referrer-policy: no-referrer
///   strict-transport-security: max-age=31536000   (https only, never for localhost)
///
/// A header the handler already set is left alone.
class Shield {
 public:
  Shield();
  static Shield none();
  /// Sends `value` for header `name`, in place of the default if there is one.
  Shield& set(std::string_view name, std::string value);
  /// Stops sending header `name`.
  Shield& remove(std::string_view name);

  void on_ignite(Ignite& ig);
  void on_response(const Request& rq, Response& rs);

 private:
  std::vector<std::pair<std::string, std::string>> headers_;
};

/// Prometheus metrics labelled by route template (never by raw path):
///   crocket_http_requests_total{method,route,status}
///   crocket_http_request_duration_seconds{method,route} (histogram)
///   crocket_http_requests_in_flight
///   crocket_extractor_failures_total{route,kind}
///   crocket_workers, crocket_workers_busy, crocket_workers_stuck (past their request's deadline)
///   crocket_requests_abandoned_total (the deadline passed, or the client left, before a worker took it)
/// Served at GET `path` (default /metrics) from on_request.
class Metrics {
 public:
  explicit Metrics(std::string path = "/metrics");

  void on_ignite(Ignite& ig);
  std::optional<Response> on_request(Request& rq);
  void on_response(const Request& rq, Response& rs);

  /// Prometheus text exposition of everything recorded so far.
  [[nodiscard]] std::string render() const;

 private:
  struct Store;
  std::string path_;
  std::shared_ptr<Store> store_;
};

}  // namespace crocket
