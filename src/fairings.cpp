#include "crocket/fairings.hpp"
#include "crocket/json.hpp"
#include "crocket/responder.hpp"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <map>
#include <mutex>
#include <tuple>

namespace crocket {

// ---- Logger -------------------------------------------------------------------

namespace {
void stderr_sink(std::string_view line) {
  static std::mutex mu;
  std::lock_guard lk(mu);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fputc('\n', stderr);
}
void field(std::string& o, std::string_view k, std::string_view v) {
  o += ',';
  json::write_string(o, k);
  o += ':';
  json::write_string(o, v);
}
std::string line_start(std::string_view level, std::string_view msg) {
  std::string o = R"({"ts":)";
  json::write_string(o, detail::iso8601_now());
  field(o, "level", level);
  field(o, "msg", msg);
  return o;
}
}  // namespace

Logger::Logger() : sink_(std::make_shared<Sink>(stderr_sink)), to_stderr_(true) {}
Logger::Logger(Sink sink) : sink_(std::make_shared<Sink>(std::move(sink))) {}

namespace {
std::string local_time_now() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  return buf;
}
}  // namespace

void Logger::on_ignite(Ignite& ig) {
  text_ = ig.config().profile == Profile::Dev;
  if (text_) {  // launch() prints the route banner
    const char* no_colour = std::getenv("NO_COLOR");
    colour_ = to_stderr_ && isatty(STDERR_FILENO) && !(no_colour && *no_colour);
    return;
  }
  auto o = line_start("info", "ignite");
  o += R"(,"routes":)" + std::to_string(ig.routes().size()) + "}";
  (*sink_)(o);
}

void Logger::on_response(const Request& rq, Response& rs) {
  auto dur = std::chrono::duration<double, std::milli>(Clock::now() - rq.received).count();
  if (text_) {
    auto paint = [&](std::string_view code, std::string_view s) {
      return colour_ ? std::format("\x1b[{}m{}\x1b[0m", code, s) : std::string(s);
    };
    std::string_view hue = rs.status >= 500 ? "31" : rs.status >= 400 ? "33" : rs.status >= 300 ? "36" : "32";
    auto o = std::format("{} {} {} {} {:.2f}ms", paint("2", local_time_now()), rq.method_text, rq.path,
                         paint(hue, std::to_string(rs.status)), dur);
    if (!rq.handler.empty()) o += std::format(" {}", rq.handler);
    if (!rs.error_code.empty()) {
      auto what = rs.error_detail.empty() ? std::string(rs.error_code)
                                          : std::format("{}: {}", rs.error_code, rs.error_detail);
      o += std::format(" {} {}", paint(hue, what), paint("2", "[" + rq.request_id + "]"));
    }
    (*sink_)(o);
    return;
  }
  std::string_view level = rs.status >= 500 ? "error" : rs.status >= 400 ? "warn" : "info";
  auto o = line_start(level, "request");
  field(o, "id", rq.request_id);
  field(o, "method", rq.method_text);
  field(o, "route", rq.route_template.empty() ? std::string_view("<unmatched>") : rq.route_template);
  if (!rq.handler.empty()) field(o, "handler", rq.handler);
  o += R"(,"status":)" + std::to_string(rs.status);
  field(o, "code", rs.error_code);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", dur);
  o += R"(,"duration_ms":)";
  o += buf;
  o += R"(,"bytes":)" + std::to_string(rs.body.size());
  field(o, "proto", rq.protocol);
  if (!rs.error_detail.empty()) field(o, "detail", rs.error_detail);
  o += '}';
  (*sink_)(o);
}

void Logger::on_shutdown() { (*sink_)(text_ ? "shutdown" : line_start("info", "shutdown") + "}"); }

// ---- Cors -----------------------------------------------------------------------

Cors Cors::deny() { return Cors{}; }
Cors Cors::allow_origins(std::vector<std::string> origins) {
  Cors c;
  c.origins_ = std::move(origins);
  return c;
}
Cors& Cors::allow_methods(std::vector<http::Method> m) { methods_ = std::move(m); return *this; }
Cors& Cors::allow_headers(std::vector<std::string> h) {
  for (auto& x : h)
    for (auto& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  headers_ = std::move(h);
  return *this;
}
Cors& Cors::expose_headers(std::vector<std::string> h) { expose_ = std::move(h); return *this; }
Cors& Cors::allow_credentials(bool yes) { credentials_ = yes; return *this; }
Cors& Cors::max_age(std::chrono::seconds a) { max_age_ = a; return *this; }

void Cors::on_ignite(Ignite& ig) {
  for (auto& o : origins_)
    if (o == "*" && credentials_) ig.fail("Cors: allow_credentials() cannot be combined with origin \"*\"");
}

bool Cors::origin_allowed(std::string_view origin) const {
  for (auto& o : origins_)
    if (o == "*" || o == origin) return true;
  return false;
}

std::string Cors::allow_origin_value(std::string_view origin) const {
  for (auto& o : origins_)
    if (o == origin) return std::string(origin);
  return "*";
}

namespace {
std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (auto& x : v) { if (!s.empty()) s += ", "; s += x; }
  return s;
}
}  // namespace

std::optional<Response> Cors::on_request(Request& rq) {
  auto origin = rq.header("origin");
  auto want = rq.header("access-control-request-method");
  if (rq.method != http::Method::Options || !origin || !want) return std::nullopt;

  rq.route_template = "<cors-preflight>";
  Response rs;
  bool ok = origin_allowed(*origin);
  auto m = http::parse_method(*want);
  ok = ok && std::ranges::find(methods_, m) != methods_.end();
  if (ok) {
    if (auto req_headers = rq.header("access-control-request-headers")) {
      std::string_view rest = *req_headers;
      while (ok && !rest.empty()) {
        auto comma = rest.find(',');
        auto h = rest.substr(0, comma);
        while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
        while (!h.empty() && h.back() == ' ') h.remove_suffix(1);
        if (!h.empty())
          ok = std::ranges::any_of(headers_, [&](auto& a) { return Headers::iequals(a, h); });
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
    }
  }
  if (!ok) {
    write_error(ApiError::forbidden("cors.denied", "cross-origin request not allowed"), rq, rs);
    return rs;
  }
  rs.status = 204;
  rs.headers.set("access-control-allow-origin", allow_origin_value(*origin));
  std::string methods;
  for (auto mm : methods_) { if (!methods.empty()) methods += ", "; methods += http::method_name(mm); }
  rs.headers.set("access-control-allow-methods", methods);
  rs.headers.set("access-control-allow-headers", join(headers_));
  rs.headers.set("access-control-max-age", std::to_string(max_age_.count()));
  if (credentials_) rs.headers.set("access-control-allow-credentials", "true");
  rs.headers.set("vary", "Origin");
  return rs;
}

void Cors::on_response(const Request& rq, Response& rs) {
  auto origin = rq.header("origin");
  if (!origin || rs.headers.contains("access-control-allow-origin") || !origin_allowed(*origin)) return;
  rs.headers.set("access-control-allow-origin", allow_origin_value(*origin));
  if (credentials_) rs.headers.set("access-control-allow-credentials", "true");
  if (!expose_.empty()) rs.headers.set("access-control-expose-headers", join(expose_));
  rs.headers.set("vary", "Origin");
}

// ---- Metrics ------------------------------------------------------------------------

namespace {
constexpr std::array<double, 12> kBuckets{0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 10};
}

struct Metrics::Store {
  struct Hist {
    std::array<std::uint64_t, kBuckets.size()> counts{};
    double sum = 0;
    std::uint64_t count = 0;
  };
  mutable std::mutex mu;
  std::map<std::tuple<std::string, std::string, int>, std::uint64_t> requests;  // method, route, status
  std::map<std::pair<std::string, std::string>, Hist> durations;               // method, route
  std::map<std::pair<std::string, std::string>, std::uint64_t> failures;       // route, kind
  const detail::Stats* stats = nullptr;
};

Metrics::Metrics(std::string path) : path_(std::move(path)), store_(std::make_shared<Store>()) {}

void Metrics::on_ignite(Ignite& ig) { store_->stats = &ig.stats(); }

std::optional<Response> Metrics::on_request(Request& rq) {
  if (rq.method != http::Method::Get || rq.path != path_) return std::nullopt;
  rq.route_template = path_;
  Response rs;
  rs.set_content_type("text/plain; version=0.0.4; charset=utf-8");
  rs.body = render();
  return rs;
}

void Metrics::on_response(const Request& rq, Response& rs) {
  std::string route = rq.route_template.empty() ? "<unmatched>" : std::string(rq.route_template);
  double secs = std::chrono::duration<double>(Clock::now() - rq.received).count();
  std::lock_guard lk(store_->mu);
  ++store_->requests[{rq.method_text, route, rs.status}];
  auto& h = store_->durations[{rq.method_text, route}];
  for (std::size_t i = 0; i < kBuckets.size(); ++i)
    if (secs <= kBuckets[i]) ++h.counts[i];
  h.sum += secs;
  ++h.count;
  if (!rs.failure_kind.empty()) ++store_->failures[{route, std::string(rs.failure_kind)}];
}

namespace {
std::string label(std::string_view v) {
  std::string o;
  for (char c : v) {
    if (c == '\\' || c == '"') o += '\\';
    if (c == '\n') { o += "\\n"; continue; }
    o += c;
  }
  return o;
}
}  // namespace

std::string Metrics::render() const {
  std::lock_guard lk(store_->mu);
  std::string o;
  o += "# HELP crocket_http_requests_total Completed HTTP requests.\n# TYPE crocket_http_requests_total counter\n";
  for (auto& [k, n] : store_->requests)
    o += "crocket_http_requests_total{method=\"" + label(std::get<0>(k)) + "\",route=\"" + label(std::get<1>(k)) +
         "\",status=\"" + std::to_string(std::get<2>(k)) + "\"} " + std::to_string(n) + "\n";
  o += "# HELP crocket_http_request_duration_seconds Request duration.\n"
       "# TYPE crocket_http_request_duration_seconds histogram\n";
  for (auto& [k, h] : store_->durations) {
    auto labels = "method=\"" + label(k.first) + "\",route=\"" + label(k.second) + "\"";
    for (std::size_t i = 0; i < kBuckets.size(); ++i) {
      char le[32];
      std::snprintf(le, sizeof le, "%g", kBuckets[i]);
      o += "crocket_http_request_duration_seconds_bucket{" + labels + ",le=\"" + le + "\"} " +
           std::to_string(h.counts[i]) + "\n";
    }
    o += "crocket_http_request_duration_seconds_bucket{" + labels + ",le=\"+Inf\"} " + std::to_string(h.count) + "\n";
    char sum[48];
    std::snprintf(sum, sizeof sum, "%.6f", h.sum);
    o += "crocket_http_request_duration_seconds_sum{" + labels + "} " + sum + "\n";
    o += "crocket_http_request_duration_seconds_count{" + labels + "} " + std::to_string(h.count) + "\n";
  }
  o += "# HELP crocket_http_requests_in_flight Requests currently being handled.\n"
       "# TYPE crocket_http_requests_in_flight gauge\n";
  o += "crocket_http_requests_in_flight " + std::to_string(store_->stats ? store_->stats->in_flight.load() : 0) + "\n";
  o += "# HELP crocket_extractor_failures_total Extractor failures by kind.\n"
       "# TYPE crocket_extractor_failures_total counter\n";
  for (auto& [k, n] : store_->failures)
    o += "crocket_extractor_failures_total{route=\"" + label(k.first) + "\",kind=\"" + label(k.second) + "\"} " +
         std::to_string(n) + "\n";
  return o;
}

}  // namespace crocket
