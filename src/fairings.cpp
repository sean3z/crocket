#include "crocket/fairings.hpp"
#include "crocket/grpc.hpp"
#include "log_hub.hpp"
#include "request_clock.hpp"
#include "crocket/json.hpp"
#include "crocket/responder.hpp"
#include "crocket/version.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
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
  if (!hub_->enabled(level)) return;
  auto end = detail::response_time(rq);  // the request's one clock read for its end
  if (rs.status < 400 && !hub_->sampled(rq.request_id) && !hub_->slow(end - rq.received)) return;
  auto dur = std::chrono::duration<double, std::milli>(end - rq.received).count();
  if (hub_->text()) {
    bool color = hub_->color();
    auto paint = [&](std::string_view code, std::string_view s) {
      return color ? std::format("\x1b[{}m{}\x1b[0m", code, s) : std::string(s);
    };
    std::string_view hue = rs.status >= 500 ? "31" : rs.status >= 400 ? "33" : rs.status >= 300 ? "36" : "32";
    auto o = std::format("{} {} {} {} {:.2f}ms", paint("2", log::detail::local_clock()), rq.method_text, rq.path,
                         paint(hue, std::to_string(rs.status)), dur);
    if (!rq.handler.empty()) o += std::format(" {}", rq.handler);
    if (rq.db_queries)
      o += paint("2", std::format(" db {:.2f}ms", std::chrono::duration<double, std::milli>(rq.db_time).count()));
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
  detail::append_iso8601(o, detail::wall_time(end));
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
  if (rq.db_queries) {  // crocket::db ran queries for this request
    key(o, R"(,"db_ms":)");
    r = std::to_chars(buf, buf + sizeof buf, std::chrono::duration<double, std::milli>(rq.db_time).count(),
                      std::chars_format::fixed, 3);
    o.append(buf, r.ptr);
    key(o, R"(,"db_wait_ms":)");
    r = std::to_chars(buf, buf + sizeof buf, std::chrono::duration<double, std::milli>(rq.db_wait).count(),
                      std::chars_format::fixed, 3);
    o.append(buf, r.ptr);
    key(o, R"(,"db_queries":)"), number(o, rq.db_queries);
  }
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
/// thread's, without a lock or a locked instruction. Its thread is the only
/// writer, so a counter goes up with a plain load and store; a sequence
/// number around each update (a seqlock) lets render() copy a series that is
/// not half updated, and add up the shards.
struct Metrics::Store {
  template <class T>
  static void bump(std::atomic<T>& a, T by = T(1)) {  // single writer: no read-modify-write needed
    a.store(a.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
  }
  struct StatusCount {
    int status;
    std::atomic<std::uint64_t> n{0};
  };
  struct Series {  // one (route, method)
    std::array<std::atomic<std::uint64_t>, kBuckets.size()> counts{};
    std::atomic<double> sum{0};
    std::atomic<std::uint64_t> count{0};
    std::vector<std::unique_ptr<StatusCount>> statuses;  // usually one or two; grows under Shard::mu
  };
  struct Shard {
    std::atomic<std::uint64_t> seq{0};  // odd while its thread updates a series
    // Taken by render(), and by its thread only to add a slot, series, status
    // or failure (the first request of its kind on this thread).
    std::mutex mu;
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

  /// Totals at the previous scrape: crocket_worker_busy_ratio and
  /// crocket_saturation cover the time since then. Under mu.
  struct Window {
    std::int64_t at_ns = 0, busy_ns = 0, queue_sum_ns = 0, lag_sum_ns = 0;
    std::uint64_t queue_count = 0, lag_count = 0;
  };
  Window last;

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

namespace {
bool label_name_ok(std::string_view n) {
  if (n.empty() || (n[0] >= '0' && n[0] <= '9')) return false;
  if (n.starts_with("__")) return false;  // reserved by Prometheus
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}
std::string_view trim(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
  return v;
}
std::string render_labels(const Metrics::Labels& ls) {
  std::string o;
  for (auto& [k, v] : ls) {
    if (!label_name_ok(k)) throw std::invalid_argument("crocket: bad metrics label name '" + k + "'");
    if (!o.empty()) o += ',';
    o += k + "=\"" + label(v) + "\"";
  }
  return o;
}
std::string host_name() {
  char buf[256] = {};
  if (::gethostname(buf, sizeof buf - 1) != 0) return "unknown";
  return buf;
}
}  // namespace

Metrics::Labels Metrics::parse_labels(std::string_view spec) {
  Labels out;
  while (!spec.empty()) {
    auto comma = spec.find(',');
    auto item = trim(spec.substr(0, comma));
    spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    if (item.empty()) continue;
    auto eq = item.find('=');
    if (eq == std::string_view::npos)
      throw std::invalid_argument("crocket: CROCKET_METRICS_LABELS item '" + std::string(item) + "' has no '='");
    auto name = trim(item.substr(0, eq));
    if (!label_name_ok(name))
      throw std::invalid_argument("crocket: bad metrics label name '" + std::string(name) + "'");
    out.emplace_back(std::string(name), std::string(trim(item.substr(eq + 1))));
  }
  return out;
}

Metrics::Metrics(std::string path, Labels constant_labels)
    : path_(std::move(path)), store_(std::make_shared<Store>()), const_labels_(render_labels(constant_labels)) {}

Metrics::Metrics(std::string path)
    : Metrics(std::move(path), [] {
        const char* env = std::getenv("CROCKET_METRICS_LABELS");
        return env ? parse_labels(env) : Labels{};
      }()) {}

void Metrics::on_ignite(Ignite& ig) {
  std::lock_guard lk(store_->mu);
  store_->stats = &ig.stats();
  store_->routes = ig.routes().data();
  store_->n_routes = ig.routes().size();
  store_->last = {};
  store_->last.at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
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
  double secs = std::chrono::duration<double>(detail::response_time(rq) - rq.received).count();
  std::size_t slot = store_->slot_of(rq);
  auto& sh = store_->shard();
  auto m = method_index(rq.method);
  Store::Series* series = sh.slots.size() > slot ? sh.slots[slot][m].get() : nullptr;  // only this thread changes it
  Store::StatusCount* st = nullptr;
  if (series)
    for (auto& sc : series->statuses)
      if (sc->status == rs.status) st = sc.get();
  if (!st || !rs.failure_kind.empty()) {  // something to add first: rare
    std::lock_guard lk(sh.mu);
    if (sh.slots.size() <= slot) sh.slots.resize(slot + 1);
    auto& sp = sh.slots[slot][m];
    if (!sp) sp = std::make_unique<Store::Series>();
    series = sp.get();
    if (!st) {
      series->statuses.push_back(std::make_unique<Store::StatusCount>());
      st = series->statuses.back().get();
      st->status = rs.status;
    }
    if (!rs.failure_kind.empty()) ++sh.failures[{slot, rs.failure_kind}];
  }
  auto seq = sh.seq.load(std::memory_order_relaxed);
  sh.seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (std::size_t i = 0; i < kBuckets.size(); ++i)
    if (secs <= kBuckets[i]) Store::bump(series->counts[i]);
  Store::bump(series->sum, secs);
  Store::bump(series->count);
  Store::bump(st->n);
  sh.seq.store(seq + 2, std::memory_order_release);
}

namespace detail {
namespace {
std::mutex g_extra_mu;
std::vector<void (*)(std::string&)>& extra_renderers() {
  static std::vector<void (*)(std::string&)> v;
  return v;
}
}  // namespace
void add_metrics_renderer(void (*render)(std::string&)) {
  std::lock_guard lk(g_extra_mu);
  for (auto f : extra_renderers())
    if (f == render) return;
  extra_renderers().push_back(render);
}
namespace {
std::vector<double (*)()>& saturation_sources() {
  static std::vector<double (*)()> v;
  return v;
}
}  // namespace
void add_saturation_source(double (*load)()) {
  std::lock_guard lk(g_extra_mu);
  for (auto f : saturation_sources())
    if (f == load) return;
  saturation_sources().push_back(load);
}
/// The largest load figure the optional components report, 0 without any.
double saturation_from_sources() {
  std::lock_guard lk(g_extra_mu);
  double m = 0;
  for (auto f : saturation_sources()) m = std::max(m, f());
  return m;
}
void render_extra_metrics(std::string& out) {
  std::lock_guard lk(g_extra_mu);
  for (auto f : extra_renderers()) f(out);
}
}  // namespace detail

namespace {

// crocket_saturation: queue wait and loop lag count as full (1.0) at these means.
constexpr double kQueueWaitTarget = 0.025;  // seconds
constexpr double kLoopLagTarget = 0.050;

/// Adds up per-thread histograms into one, and writes it.
struct LatencyTotals {
  std::array<std::uint64_t, detail::LatencyHistogram::kBoundsNs.size()> buckets{};
  std::uint64_t count = 0;
  std::int64_t sum_ns = 0;
  void add(const detail::LatencyHistogram& h) {
    // count first: it is written last, so the buckets hold at least that many
    // (and the cumulative buckets are capped at it below).
    count += h.count.load(std::memory_order_acquire);
    sum_ns += h.sum_ns.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < buckets.size(); ++i) buckets[i] += h.buckets[i].load(std::memory_order_relaxed);
  }
  void write(std::string& o, std::string_view name, std::string_view help) const {
    o += "# HELP "; o += name; o += ' '; o += help; o += "\n# TYPE "; o += name; o += " histogram\n";
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
      cum += buckets[i];
      char le[32];
      std::snprintf(le, sizeof le, "%g", double(detail::LatencyHistogram::kBoundsNs[i]) / 1e9);
      o += name; o += "_bucket{le=\""; o += le; o += "\"} " + std::to_string(std::min(cum, count)) + "\n";
    }
    o += name; o += "_bucket{le=\"+Inf\"} " + std::to_string(count) + "\n";
    char sum[48];
    std::snprintf(sum, sizeof sum, "%.6f", double(sum_ns) / 1e9);
    o += name; o += "_sum "; o += sum; o += "\n";
    o += name; o += "_count " + std::to_string(count) + "\n";
  }
};

std::string fixed(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  return b;
}

}  // namespace

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
      std::lock_guard slk(shp->mu);  // no series or status is added meanwhile
      struct Copy {
        Hist h;
        std::vector<std::pair<int, std::uint64_t>> statuses;
      };
      std::vector<std::pair<std::size_t, Copy>> copies;  // (slot * kMethods + method, counters)
      // A consistent copy of this shard: again if its thread updated it meanwhile.
      while (true) {
        auto before = shp->seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        copies.clear();
        for (std::size_t slot = 0; slot < shp->slots.size(); ++slot)
          for (std::size_t m = 0; m < kMethods; ++m) {
            auto& series = shp->slots[slot][m];
            if (!series) continue;
            Copy c;
            for (std::size_t i = 0; i < kBuckets.size(); ++i) c.h.counts[i] = series->counts[i].load(std::memory_order_relaxed);
            c.h.sum = series->sum.load(std::memory_order_relaxed);
            c.h.count = series->count.load(std::memory_order_relaxed);
            for (auto& sc : series->statuses) c.statuses.emplace_back(sc->status, sc->n.load(std::memory_order_relaxed));
            copies.emplace_back(slot * kMethods + m, std::move(c));
          }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (shp->seq.load(std::memory_order_relaxed) == before) break;
      }
      for (auto& [key, c] : copies) {
        if (c.h.count == 0) continue;  // added a moment ago, not counted yet
        std::string method(method_label(key % kMethods)), route = store_->slot_label(key / kMethods);
        for (auto& [status, n] : c.statuses)
          if (n) requests[{method, route, status}] += n;
        auto& h = durations[{method, route}];
        for (std::size_t i = 0; i < kBuckets.size(); ++i) h.counts[i] += c.h.counts[i];
        h.sum += c.h.sum;
        h.count += c.h.count;
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
  if (const auto* st = store_->stats) {
    unsigned n = st->workers.load(), busy = 0, stuck = 0;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    for (unsigned i = 0; i < n; ++i) {
      auto deadline = st->worker_deadline[i].load(std::memory_order_relaxed);
      if (deadline == 0) continue;
      ++busy;
      if (now > deadline) ++stuck;
    }
    o += "# HELP crocket_workers Handler threads.\n# TYPE crocket_workers gauge\ncrocket_workers " + std::to_string(n) + "\n";
    o += "# HELP crocket_workers_busy Workers running a handler now.\n# TYPE crocket_workers_busy gauge\n"
         "crocket_workers_busy " + std::to_string(busy) + "\n";
    o += "# HELP crocket_workers_stuck Workers still running a handler after its request's deadline: it ignores "
         "the deadline, and holds the worker.\n# TYPE crocket_workers_stuck gauge\ncrocket_workers_stuck " +
         std::to_string(stuck) + "\n";
    o += "# HELP crocket_requests_abandoned_total Requests whose handler never ran: the deadline passed, or the "
         "client went away, while they waited for a worker.\n# TYPE crocket_requests_abandoned_total counter\n"
         "crocket_requests_abandoned_total " + std::to_string(st->abandoned.load()) + "\n";
    o += "# HELP crocket_connections Open connections (Config::limits.max_connections).\n"
         "# TYPE crocket_connections gauge\ncrocket_connections " + std::to_string(st->connections.load()) + "\n";
    o += "# HELP crocket_connections_per_ip_closed_total Connections closed on arrival: their address had "
         "max_connections_per_ip open.\n# TYPE crocket_connections_per_ip_closed_total counter\n"
         "crocket_connections_per_ip_closed_total " + std::to_string(st->connections_per_ip_closed.load()) + "\n";
    o += "# HELP crocket_request_body_bytes Request bodies held in memory now (Config::limits.max_body_memory).\n"
         "# TYPE crocket_request_body_bytes gauge\ncrocket_request_body_bytes " + std::to_string(st->body_bytes.load()) + "\n";
    o += "# HELP crocket_requests_shed_total Requests answered 503 server.busy before a handler ran, by the limit "
         "they hit.\n# TYPE crocket_requests_shed_total counter\n"
         "crocket_requests_shed_total{reason=\"max_in_flight\"} " + std::to_string(st->shed_in_flight.load()) + "\n"
         "crocket_requests_shed_total{reason=\"max_body_memory\"} " + std::to_string(st->shed_body_memory.load()) + "\n";

    // Busy time: finished jobs, plus the running ones so far.
    std::int64_t busy_ns = 0;
    LatencyTotals queue;
    for (unsigned i = 0; i < n; ++i) {
      auto& u = st->worker_usage[i];
      busy_ns += u.busy_ns.load(std::memory_order_relaxed);
      if (auto since = u.since_ns.load(std::memory_order_relaxed); since && since < now) busy_ns += now - since;
      queue.add(u.queue_wait);
    }
    LatencyTotals lag;
    lag.add(st->loop_lag);
    o += "# HELP crocket_worker_busy_seconds_total Time workers spent running handlers, all workers together.\n"
         "# TYPE crocket_worker_busy_seconds_total counter\ncrocket_worker_busy_seconds_total " +
         fixed(double(busy_ns) / 1e9) + "\n";
    queue.write(o, "crocket_request_queue_wait_seconds",
                "Time a request waited for a worker: from being queued until a worker took it.");
    lag.write(o, "crocket_event_loop_lag_seconds",
              "How late each event loop's 100 ms tick fired: how long something held the loop.");

    // Since the previous scrape (since ignite, for the first).
    double busy_ratio = 0, queue_mean = 0, lag_mean = 0;
    {
      std::lock_guard lk(store_->mu);
      auto& w = store_->last;
      auto dt = now - w.at_ns;
      if (n && w.at_ns && dt > 0) busy_ratio = std::clamp(double(busy_ns - w.busy_ns) / (double(dt) * n), 0.0, 1.0);
      if (queue.count > w.queue_count) queue_mean = double(queue.sum_ns - w.queue_sum_ns) / double(queue.count - w.queue_count) / 1e9;
      if (lag.count > w.lag_count) lag_mean = double(lag.sum_ns - w.lag_sum_ns) / double(lag.count - w.lag_count) / 1e9;
      w = {now, busy_ns, queue.sum_ns, lag.sum_ns, queue.count, lag.count};
    }
    double saturation = std::max({busy_ratio, std::min(1.0, std::max(0.0, queue_mean) / kQueueWaitTarget),
                                  std::min(1.0, std::max(0.0, lag_mean) / kLoopLagTarget),
                                  std::clamp(detail::saturation_from_sources(), 0.0, 1.0)});
    o += "# HELP crocket_worker_busy_ratio Share of worker time spent running handlers since the previous scrape "
         "(0..1).\n# TYPE crocket_worker_busy_ratio gauge\ncrocket_worker_busy_ratio " + fixed(busy_ratio) + "\n";
    o += "# HELP crocket_saturation How close this instance is to its ceiling since the previous scrape (0..1): the "
         "largest of the worker busy ratio, the database pool in use, mean queue wait over 25 ms and mean event loop "
         "lag over 50 ms.\n# TYPE crocket_saturation gauge\ncrocket_saturation " + fixed(saturation) + "\n";
  }
  o += "# HELP crocket_extractor_failures_total Extractor failures by kind.\n"
       "# TYPE crocket_extractor_failures_total counter\n";
  for (auto& [k, n] : failures)
    o += "crocket_extractor_failures_total{route=\"" + label(k.first) + "\",kind=\"" + label(k.second) + "\"} " +
         std::to_string(n) + "\n";
  o += "# HELP crocket_build_info crocket version, commit and compiler, and this host. Always 1.\n"
       "# TYPE crocket_build_info gauge\n"
       "crocket_build_info{version=\"" + label(version::string) + "\",commit=\"" + label(version::commit) +
       "\",compiler=\"" + label(version::compiler) + "\",hostname=\"" + label(host_name()) + "\"} 1\n";
  detail::render_extra_metrics(o);
  if (const_labels_.empty()) return o;
  // Constant labels on every sample line: name{a="b"} v -> name{a="b",k="v"} v.
  std::string out;
  out.reserve(o.size() + o.size() / 4);
  std::string_view rest = o;
  while (!rest.empty()) {
    auto nl = rest.find('\n');
    auto line = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
    if (line.empty() || line[0] == '#') {
      out += line;
    } else if (auto br = line.find('{'); br != std::string_view::npos && br < line.find(' ')) {
      out += line.substr(0, br + 1);
      out += const_labels_;
      out += ',';
      out += line.substr(br + 1);
    } else {
      auto sp = line.find(' ');
      out += line.substr(0, sp);
      out += '{';
      out += const_labels_;
      out += '}';
      out += line.substr(sp);
    }
    out += '\n';
  }
  return out;
}

}  // namespace crocket
