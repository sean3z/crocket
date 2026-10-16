#pragma once
// Standard fairings. All are short, synchronous and thread-safe.

#include "crocket/app.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// Prometheus metrics labeled by route template (never by raw path):
///   crocket_http_requests_total{method,route,status}
///   crocket_http_request_duration_seconds{method,route} (histogram)
///   crocket_http_requests_in_flight
///   crocket_extractor_failures_total{route,kind}
///   crocket_workers, crocket_workers_busy, crocket_workers_stuck (past their request's deadline)
///   crocket_requests_abandoned_total (the deadline passed, or the client left, before a worker took it)
///   crocket_requests_shed_total{reason} (503 server.busy before a handler ran)
///   crocket_worker_busy_seconds_total, crocket_worker_busy_ratio (since the previous scrape)
///   crocket_request_queue_wait_seconds (histogram: queued until a worker took it)
///   crocket_event_loop_lag_seconds (histogram: how late each loop's 100 ms tick fires)
///   crocket_saturation (0..1, for autoscaling; see README "Autoscaling on crocket_saturation")
///   crocket_build_info{version,commit,compiler,hostname} 1
///   crocket_db_seconds{table,op} (histogram), crocket_db_pool_connections{state}, crocket_db_pool_size,
///   crocket_db_pool_wait_seconds (histogram): with crocket::db
/// Served at GET `path` (default /metrics) from on_request.
///
/// Constant labels are added to every series. By default they come from
/// CROCKET_METRICS_LABELS, a comma-separated list such as
/// `pod=web-7f9c,zone=a` (in Kubernetes, fill it from the Downward API).
namespace detail {
/// Optional components (crocket::db) add their series to every Metrics
/// render through this. Registering the same function twice is a no-op.
void add_metrics_renderer(void (*render)(std::string&));
void render_extra_metrics(std::string& out);
/// Optional components (crocket::db) add a 0..1 load figure to crocket_saturation
/// through this. Registering the same function twice is a no-op.
void add_saturation_source(double (*load)());
}  // namespace detail

class Metrics {
 public:
  using Labels = std::vector<std::pair<std::string, std::string>>;
  explicit Metrics(std::string path = "/metrics");
  /// Explicit constant labels; CROCKET_METRICS_LABELS is not read.
  Metrics(std::string path, Labels constant_labels);

  /// Parses `name=value,name=value`. Throws std::invalid_argument on a bad name
  /// or a missing `=`; spaces around items are ignored.
  static Labels parse_labels(std::string_view spec);

  void on_ignite(Ignite& ig);
  std::optional<Response> on_request(Request& rq);
  void on_response(const Request& rq, Response& rs);

  /// Prometheus text exposition of everything recorded so far.
  [[nodiscard]] std::string render() const;

 private:
  struct Store;
  std::string path_;
  std::shared_ptr<Store> store_;
  std::string const_labels_;  // rendered: name="value",name="value"
};

/// The OpenAPI 3.1 description of the app's HTTP routes, generated from the
/// handlers themselves: path captures, Query<T> and Header<...> parameters,
/// Json<T> bodies, Auth, and what each handler returns, with JSON Schemas for
/// the types as crocket reads and writes them. Served at GET `path`; with
/// `docs` set, an interactive page (Swagger UI) there too.
///
///   app.attach(OpenApi{{.title = "Orders", .version = "1.4.0", .docs = "/docs"}});
struct OpenApiOptions {
  std::string title = "API";
  std::string version = "0.1.0";
  std::string description = {};
  std::string path = "/openapi.json";
  std::string docs = {};  // "/docs": Swagger UI, loaded from cdn.jsdelivr.net
};
class OpenApi {
 public:
  using Options = OpenApiOptions;
  explicit OpenApi(Options o = {});

  void on_ignite(Ignite& ig);
  std::optional<Response> on_request(Request& rq);

 private:
  Options o_;
  std::shared_ptr<const std::string> doc_;
};

/// The OpenAPI document for `app` (igniting it first), for writing openapi.json
/// at build time or checking it into a repository:
///   if (argv[1] == "--openapi"sv) { std::cout << crocket::openapi_document(app, {.title = "Orders"}); return 0; }
std::string openapi_document(Crocket& app, const OpenApi::Options& o = {});

}  // namespace crocket
