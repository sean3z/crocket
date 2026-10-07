#include "crocket/fairings.hpp"
#include "crocket/grpc.hpp"
#include "log_hub.hpp"
#include "crocket/json.hpp"
#include "crocket/responder.hpp"

#include <unistd.h>

#include <array>
#include <charconv>
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
void field(std::string& o, std::string_view k, std::string_view v) {
  o += ',';
  json::write_string(o, k);
  o += ':';
  json::write_string(o, v);
}
std::string line_start(std::string_view level, std::string_view msg) {
  std::string o = R"({"ts":")";
  detail::append_iso8601_now(o);
  o += '"';
  field(o, "level", level);
  field(o, "msg", msg);
  return o;
}

// The request line is built once per request: constant keys go in as they are
// (no escaping to do), values through the JSON escaper, numbers via to_chars.
void key(std::string& o, std::string_view k) {  // `k` is ,"name":
  o += k;
}
void value(std::string& o, std::string_view v) { json::write_string(o, v); }
template <class N>
void number(std::string& o, N n) {
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, n);
  o.append(buf, r.ptr);
}
}  // namespace

void Logger::on_ignite(Ignite& ig) {
  hub_ = ig.log_hub();
  if (hub_->text() || !hub_->enabled(log::Level::info)) return;  // launch() prints the routes in dev
  hub_->write(line_start("info", "ignite") + R"(,"routes":)" + std::to_string(ig.routes().size()) + "}");
}

void Logger::on_response(const Request& rq, Response& rs) {
  if (!hub_) return;
  auto level = rs.status >= 500 ? log::Level::error : rs.status >= 400 ? log::Level::warn : log::Level::info;
  if (!hub_->enabled(level) || (rs.status < 400 && !hub_->sampled(rq.request_id))) return;
  auto dur = std::chrono::duration<double, std::milli>(Clock::now() - rq.received).count();
  if (hub_->text()) {
    bool colour = hub_->colour();
    auto paint = [&](std::string_view code, std::string_view s) {
      return colour ? std::format("\x1b[{}m{}\x1b[0m", code, s) : std::string(s);
    };
    std::string_view hue = rs.status >= 500 ? "31" : rs.status >= 400 ? "33" : rs.status >= 300 ? "36" : "32";
    auto o = std::format("{} {} {} {} {:.2f}ms", paint("2", log::detail::local_clock()), rq.method_text, rq.path,
                         paint(hue, std::to_string(rs.status)), dur);
    if (!rq.handler.empty()) o += std::format(" {}", rq.handler);
    if (!rq.subject.empty()) o += std::format(" {}", paint("2", "as " + rq.subject));
    if (!rs.error_code.empty()) {
      auto what = rs.error_detail.empty() ? std::string(rs.error_code)
                                          : std::format("{}: {}", rs.error_code, rs.error_detail);
      o += std::format(" {} {}", paint(hue, what), paint("2", "[" + rq.request_id + "]"));
    }
    hub_->write(std::move(o));
    return;
  }
  constexpr std::string_view names[] = {R"(,"level":"debug")", R"(,"level":"info")", R"(,"level":"warn")",
                                       R"(,"level":"error")"};
  thread_local std::string o;  // reused: the hub copies the line, so nothing is allocated per request
  o.clear();
  o += R"({"ts":")";
  detail::append_iso8601_now(o);
  o += '"';
  o += names[int(level)];
  o += R"(,"msg":"request","request_id":)";
  value(o, rq.request_id);
  if (!rq.trace_id.empty()) key(o, R"(,"trace_id":)"), value(o, rq.trace_id);
  key(o, R"(,"method":)"), value(o, rq.method_text);
  key(o, R"(,"route":)"), value(o, rq.route_template.empty() ? std::string_view("<unmatched>") : rq.route_template);
  if (!rq.handler.empty()) key(o, R"(,"handler":)"), value(o, rq.handler);
  key(o, R"(,"status":)"), number(o, rs.status);
  key(o, R"(,"code":)"), value(o, rs.error_code);
  key(o, R"(,"duration_ms":)");
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, dur, std::chars_format::fixed, 3);
  o.append(buf, r.ptr);
  key(o, R"(,"bytes_in":)"), number(o, rq.body.size());
  key(o, R"(,"bytes_out":)"), number(o, rs.body.size());
  key(o, R"(,"proto":)"), value(o, rq.protocol);
  if (!rq.remote_addr.empty()) key(o, R"(,"client":)"), value(o, rq.remote_addr);
  if (auto ua = rq.header("user-agent")) key(o, R"(,"user_agent":)"), value(o, ua->substr(0, 256));
  if (!rq.subject.empty()) key(o, R"(,"subject":)"), value(o, rq.subject);
  if (!rs.error_detail.empty()) key(o, R"(,"detail":)"), value(o, rs.error_detail);
  o += '}';
  hub_->write(o);
}

void Logger::on_shutdown() {
  if (!hub_) return;
  if (hub_->enabled(log::Level::info)) hub_->write(hub_->text() ? "shutdown" : line_start("info", "shutdown") + "}");
  hub_->flush();
}

// ---- Cors -----------------------------------------------------------------------

// ---- Shield ----------------------------------------------------------------------

Shield::Shield()
    : headers_{{"x-content-type-options", "nosniff"},
               {"x-frame-options", "DENY"},
               {"content-security-policy", "default-src 'none'; frame-ancestors 'none'"},
               {"referrer-policy", "no-referrer"},
               {"strict-transport-security", "max-age=31536000"}} {}

Shield Shield::none() {
  Shield s;
  s.headers_.clear();
  return s;
}

Shield& Shield::set(std::string_view name, std::string value) {
  remove(name);
  std::string lower(name);
  for (auto& c : lower)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  headers_.emplace_back(std::move(lower), std::move(value));
  return *this;
}

Shield& Shield::remove(std::string_view name) {
  std::erase_if(headers_, [&](auto& h) { return Headers::iequals(h.first, name); });
  return *this;
}

void Shield::on_ignite(Ignite& ig) {
  for (auto& [k, v] : headers_)
    if (!http::valid_header_name(k) || !http::valid_header_value(v))
      ig.fail("Shield: header '" + k + "' has a name or value not allowed in HTTP headers");
}

void Shield::on_response(const Request& rq, Response& rs) {
  if (auto ct = rs.headers.get("content-type"); ct && grpc::is_grpc_content_type(*ct)) return;
  for (auto& [k, v] : headers_) {
    if (rs.headers.contains(k)) continue;
    if (k == "strict-transport-security") {
      // Over plain http a browser ignores it; on localhost it would pin every
      // local development server to https.
      auto host = rq.header("host").value_or("");
      bool local = host.starts_with("localhost") || host.starts_with("127.") || host.starts_with("[::1]");
      if (rq.scheme != "https" || local) continue;
    }
    rs.headers.set(k, v);
  }
}

// ---- Cors ----------------------------------------------------------------------

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

namespace {

// Methods get a fixed label each; anything else a client sends is "_OTHER", so
// made-up methods cannot add series without bound.
constexpr std::size_t kMethods = 8;
std::size_t method_index(http::Method m) {
  return m == http::Method::Unknown ? kMethods - 1 : std::size_t(m);
}
std::string_view method_label(std::size_t i) {
  return i == kMethods - 1 ? std::string_view("_OTHER") : http::method_name(http::Method(i));
}

std::atomic<std::uint64_t> g_next_store_id{1};

/// A Prometheus label value, escaped.
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

/// Counters live in one shard per thread: a request touches only its own
/// thread's, under a mutex no other request takes. render() adds them up.
struct Metrics::Store {
  struct Series {  // one (route, method)
    std::array<std::uint64_t, kBuckets.size()> counts{};
    double sum = 0;
    std::uint64_t count = 0;
    std::vector<std::pair<int, std::uint64_t>> statuses;  // usually one or two
  };
  struct Shard {
    std::mutex mu;  // taken by its thread and by render(), never by other requests
    std::vector<std::array<std::unique_ptr<Series>, kMethods>> slots;  // [route slot][method]
    std::map<std::pair<std::size_t, std::string_view>, std::uint64_t> failures;  // slot, kind (static)
  };

  const std::uint64_t id = g_next_store_id.fetch_add(1);  // never reused, unlike an address
  mutable std::mutex mu;  // the shard list and extra slots
  std::vector<std::unique_ptr<Shard>> shards;
  const RouteDef* routes = nullptr;  // slot i < n_routes is routes[i]
  std::size_t n_routes = 0;          // slot n_routes is "<unmatched>"
  std::vector<std::string> extra;    // later slots: templates a fairing set ("/metrics")
  const detail::Stats* stats = nullptr;

  Shard& shard() {
    // Per thread, the shard of each store this thread has recorded into.
    thread_local std::vector<std::pair<std::uint64_t, Shard*>> mine;
    for (auto& [store, sh] : mine)
      if (store == id) return *sh;
    std::lock_guard lk(mu);
    shards.push_back(std::make_unique<Shard>());
    if (mine.size() >= 16) mine.clear();  // entries of stores that are gone
    mine.emplace_back(id, shards.back().get());
    return *shards.back();
  }

  std::size_t slot_of(const Request& rq) {
    if (rq.route && routes && rq.route >= routes && rq.route < routes + n_routes) return std::size_t(rq.route - routes);
    if (rq.route_template.empty()) return n_routes;
    std::lock_guard lk(mu);  // a template a fairing set: rare (a metrics scrape, say)
    for (std::size_t i = 0; i < extra.size(); ++i)
      if (extra[i] == rq.route_template) return n_routes + 1 + i;
    extra.emplace_back(rq.route_template);
    return n_routes + extra.size();
  }

  std::string slot_label(std::size_t slot) const {
    if (slot < n_routes) return routes[slot].path;
    if (slot == n_routes) return "<unmatched>";
    return extra[slot - n_routes - 1];
  }
};

Metrics::Metrics(std::string path) : path_(std::move(path)), store_(std::make_shared<Store>()) {}

void Metrics::on_ignite(Ignite& ig) {
  std::lock_guard lk(store_->mu);
  store_->stats = &ig.stats();
  store_->routes = ig.routes().data();
  store_->n_routes = ig.routes().size();
}

std::optional<Response> Metrics::on_request(Request& rq) {
  if (rq.method != http::Method::Get || rq.path != path_) return std::nullopt;
  rq.route_template = path_;
  Response rs;
  rs.set_content_type("text/plain; version=0.0.4; charset=utf-8");
  rs.body = render();
  return rs;
}

void Metrics::on_response(const Request& rq, Response& rs) {
  double secs = std::chrono::duration<double>(Clock::now() - rq.received).count();
  std::size_t slot = store_->slot_of(rq);
  auto& sh = store_->shard();
  std::lock_guard lk(sh.mu);
  if (sh.slots.size() <= slot) sh.slots.resize(slot + 1);
  auto& series = sh.slots[slot][method_index(rq.method)];
  if (!series) series = std::make_unique<Store::Series>();
  for (std::size_t i = 0; i < kBuckets.size(); ++i)
    if (secs <= kBuckets[i]) ++series->counts[i];
  series->sum += secs;
  ++series->count;
  auto st = std::ranges::find(series->statuses, rs.status, &std::pair<int, std::uint64_t>::first);
  if (st != series->statuses.end()) ++st->second;
  else series->statuses.emplace_back(rs.status, 1);
  if (!rs.failure_kind.empty()) ++sh.failures[{slot, rs.failure_kind}];
}

std::string Metrics::render() const {
  // Add up every thread's shard, in the order the output has always had.
  struct Hist {
    std::array<std::uint64_t, kBuckets.size()> counts{};
    double sum = 0;
    std::uint64_t count = 0;
  };
  std::map<std::tuple<std::string, std::string, int>, std::uint64_t> requests;  // method, route, status
  std::map<std::pair<std::string, std::string>, Hist> durations;               // method, route
  std::map<std::pair<std::string, std::string>, std::uint64_t> failures;       // route, kind
  {
    std::lock_guard lk(store_->mu);
    for (auto& shp : store_->shards) {
      std::lock_guard slk(shp->mu);
      for (std::size_t slot = 0; slot < shp->slots.size(); ++slot)
        for (std::size_t m = 0; m < kMethods; ++m) {
          auto& series = shp->slots[slot][m];
          if (!series) continue;
          std::string method(method_label(m)), route = store_->slot_label(slot);
          for (auto& [status, n] : series->statuses) requests[{method, route, status}] += n;
          auto& h = durations[{method, route}];
          for (std::size_t i = 0; i < kBuckets.size(); ++i) h.counts[i] += series->counts[i];
          h.sum += series->sum;
          h.count += series->count;
        }
      for (auto& [k, n] : shp->failures) failures[{store_->slot_label(k.first), std::string(k.second)}] += n;
    }
  }
  std::string o;
  o += "# HELP crocket_http_requests_total Completed HTTP requests.\n# TYPE crocket_http_requests_total counter\n";
  for (auto& [k, n] : requests)
    o += "crocket_http_requests_total{method=\"" + label(std::get<0>(k)) + "\",route=\"" + label(std::get<1>(k)) +
         "\",status=\"" + std::to_string(std::get<2>(k)) + "\"} " + std::to_string(n) + "\n";
  o += "# HELP crocket_http_request_duration_seconds Request duration.\n"
       "# TYPE crocket_http_request_duration_seconds histogram\n";
  for (auto& [k, h] : durations) {
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
  o += "# HELP crocket_log_lines_dropped_total Log lines discarded because the writer fell behind.\n"
       "# TYPE crocket_log_lines_dropped_total counter\n";
  o += "crocket_log_lines_dropped_total " + std::to_string(store_->stats ? store_->stats->log_dropped.load() : 0) + "\n";
  o += "# HELP crocket_extractor_failures_total Extractor failures by kind.\n"
       "# TYPE crocket_extractor_failures_total counter\n";
  for (auto& [k, n] : failures)
    o += "crocket_extractor_failures_total{route=\"" + label(k.first) + "\",kind=\"" + label(k.second) + "\"} " +
         std::to_string(n) + "\n";
  return o;
}

}  // namespace crocket
