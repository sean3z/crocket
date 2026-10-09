#include "crocket/app.hpp"
#include "crocket/fairings.hpp"
#include "crocket/grpc.hpp"
#include "crocket/responder.hpp"
#include "engine.hpp"
#include "conditional.hpp"
#include "identity.hpp"
#include "log_hub.hpp"
#include "router.hpp"

#include <algorithm>
#include <thread>
#include <semaphore>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace crocket {

Config Config::dev() {
  Config c;
  c.profile = Profile::Dev;
  c.debug_routes = true;
  c.request_timeout = std::chrono::hours(1);
  c.log.level = log::Level::debug;
  c.drain_timeout = std::chrono::seconds(1);
  return c;
}

Config Config::from_env() {
  const char* v = std::getenv("CROCKET_PROFILE");
  std::string_view p = v ? v : "";
  if (p.empty() || p == "release") return {};
  if (p == "dev") return dev();
  throw std::invalid_argument("CROCKET_PROFILE=" + std::string(p) + ": unknown profile (use dev or release)");
}

Crocket::Crocket(Config cfg) : core_(std::make_unique<detail::Core>()) {
  core_->config = std::move(cfg);
  core_->log = std::make_shared<log::detail::Hub>(core_->config.log, core_->config.profile == Profile::Dev,
                                                  &core_->stats.log_dropped);
}
Crocket::Crocket(Crocket&&) noexcept = default;
Crocket& Crocket::operator=(Crocket&&) noexcept = default;
Crocket::~Crocket() {
  if (core_) log::detail::clear_default(core_->log.get());
}

void Crocket::flush_logs() const {
  if (core_ && core_->log) core_->log->flush();
}

Crocket& Crocket::mount(std::string_view base, Routes routes, Mode mode) & {
  if (core_->ignited)
    core_->build_errors.push_back("mount(\"" + std::string(base) + "\") after ignite");
  core_->mounts.push_back({std::string(base), std::move(routes), mode});
  return *this;
}

Crocket& Crocket::configure(Config cfg) & {
  core_->config = std::move(cfg);
  return *this;
}

namespace {

/// The trace id of a W3C traceparent ("00-<32 hex>-<16 hex>-<2 hex>"), or "".
std::string trace_id_of(std::string_view tp) {
  auto hex = [](std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  };
  if (tp.size() < 55 || tp[2] != '-' || tp[35] != '-' || tp[52] != '-') return {};
  auto version = tp.substr(0, 2), trace = tp.substr(3, 32), parent = tp.substr(36, 16), flags = tp.substr(53, 2);
  if (!hex(version) || version == "ff" || !hex(trace) || !hex(parent) || !hex(flags)) return {};
  if (version == "00" && tp.size() != 55) return {};
  if (trace.find_first_not_of('0') == std::string_view::npos || parent.find_first_not_of('0') == std::string_view::npos)
    return {};
  return std::string(trace);
}

}  // namespace

namespace {

bool valid_request_id(std::string_view id) {
  if (id.empty() || id.size() > 128) return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

std::string describe(const RouteDef& r) {
  return std::string(http::method_name(r.method)) + " " + r.path + " (" + std::string(r.handler) + ")";
}

/// The dev profile's launch banner: every route, aligned, on stderr.
void print_banner(const Routes& routes) {
  std::size_t width = 0;
  for (auto& r : routes) width = std::max(width, r.path.size());
  std::fprintf(stderr, "crocket: dev profile (error bodies include details; not for production)\n");
  for (auto& r : routes) {
    auto rank = r.rank ? " (rank " + std::to_string(r.rank) + ")" : std::string();
    std::fprintf(stderr, "  %-7s %-*s  %.*s%s\n", http::method_name(r.method).data(), int(width), r.path.c_str(),
                 int(r.handler.size()), r.handler.data(), rank.c_str());
  }
}

/// "helloworld", "acme.billing.v1", or "" (no package).
bool valid_package(std::string_view p) {
  if (p.empty()) return true;
  bool start = true;
  for (char c : p) {
    if (c == '.') {
      if (start) return false;
      start = true;
      continue;
    }
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!alpha && !(c >= '0' && c <= '9' && !start)) return false;
    start = false;
  }
  return !start;
}

/// gRPC's Timeout: up to 8 digits and a unit (H, M, S, m, u, n).
std::optional<Clock::duration> parse_grpc_timeout(std::string_view t) {
  if (t.size() < 2 || t.size() > 9) return std::nullopt;
  std::int64_t n = 0;
  auto digits = t.substr(0, t.size() - 1);
  auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
  if (ec != std::errc{} || p != digits.data() + digits.size() || n < 0) return std::nullopt;
  using namespace std::chrono;
  switch (t.back()) {
    case 'H': return duration_cast<Clock::duration>(hours(n));
    case 'M': return duration_cast<Clock::duration>(minutes(n));
    case 'S': return duration_cast<Clock::duration>(seconds(n));
    case 'm': return duration_cast<Clock::duration>(milliseconds(n));
    case 'u': return duration_cast<Clock::duration>(microseconds(n));
    case 'n': return duration_cast<Clock::duration>(nanoseconds(n));
    default: return std::nullopt;
  }
}

RouteDef builtin_route(std::string path, std::string_view handler, detail::Builtin b) {
  RouteDef d;
  d.method = http::Method::Get;
  d.path = std::move(path);
  d.handler = handler;
  d.builtin = b;
  return d;
}

}  // namespace

std::expected<void, IgniteError> Crocket::ignite() {
  auto& c = *core_;
  if (c.ignited) return {};
  std::vector<std::string> errors = c.build_errors;

  // 1. Assemble the route table: mount prefixes + built-ins.
  Routes routes;
  for (auto& [base_in, list, mode] : c.mounts) {
    if (mode == Mode::Grpc) {
      const std::string& package = base_in;
      if (!valid_package(package)) {
        errors.push_back("gRPC mount \"" + package + "\": the first argument of a Mode::Grpc mount is a protobuf "
                         "package such as \"helloworld\" or \"acme.v1\" (or \"\" for none), not a path");
        continue;
      }
      for (auto r : list) {
        if (r.mode != Mode::Grpc) {
          errors.push_back(describe(r) + " is an HTTP route mounted with Mode::Grpc; mount its scope without Mode::Grpc");
          continue;
        }
        r.path = "/" + (package.empty() ? std::string() : package + ".") + std::string(r.service) + "/" +
                 std::string(r.rpc);
        routes.push_back(std::move(r));
      }
      continue;
    }
    std::string base = base_in;
    if (base.empty() || base.front() != '/') {
      errors.push_back("mount base \"" + base + "\" must start with '/'");
      continue;
    }
    if (base.find_first_of("{}") != std::string::npos) {
      errors.push_back("mount base \"" + base + "\" must not contain captures");
      continue;
    }
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    for (auto r : list) {
      if (r.mode == Mode::Grpc) {
        errors.push_back(std::string(r.handler) + " is a [[= grpc::rpc]] method; mount its scope with Mode::Grpc: "
                         ".mount(\"<package>\", reflect_routes<...>(), crocket::Mode::Grpc)");
        continue;
      }
      if (base != "/") r.path = r.path == "/" ? base : base + r.path;
      std::vector<http::Segment> segs;
      if (auto err = http::parse_template(r.path, segs); !err.empty())
        errors.push_back(describe(r) + ": " + err);
      else if (segs.size() > detail::Router::kMaxSegments)
        errors.push_back(describe(r) + ": more than " + std::to_string(detail::Router::kMaxSegments) + " path segments");
      else if (std::ranges::count_if(segs, &http::Segment::capture) > std::ptrdiff_t(Captures::capacity))
        errors.push_back(describe(r) + ": more than " + std::to_string(Captures::capacity) + " captures");
      routes.push_back(std::move(r));
    }
  }
  routes.push_back(builtin_route("/healthz", "crocket::healthz", detail::Builtin::Healthz));
  routes.push_back(builtin_route("/readyz", "crocket::readyz", detail::Builtin::Readyz));
  if (c.config.debug_routes)
    routes.push_back(builtin_route("/__routes", "crocket::routes", detail::Builtin::Routes));

  // 2. Proxies and hosts must be well-formed.
  c.trusted_proxies.clear();
  for (auto& p : c.config.trusted_proxies) {
    if (auto r = detail::parse_ip_range(p)) c.trusted_proxies.push_back(*r);
    else errors.push_back("trusted_proxies entry \"" + p + "\" is not an IP address or CIDR range (\"10.0.0.0/8\")");
  }
  for (auto& h : c.config.allowed_hosts)
    if (auto why = detail::host_pattern_problem(h); !why.empty())
      errors.push_back("allowed_hosts entry \"" + h + "\" " + why);

  // 3. Every State<T> a route needs must be managed.
  std::set<std::string> seen;
  for (auto& r : routes)
    for (auto& need : r.needs)
      if (!c.state.find(need.key)) {
        auto msg = describe(r) + " requires State<" + std::string(need.name) + ">, but " + std::string(need.name) +
                   " is not managed; add .manage(" + std::string(need.name) + "{...})";
        if (seen.insert(msg).second) errors.push_back(std::move(msg));
      }

  // 4. Same method + same shape + same rank can never be disambiguated.
  std::map<std::tuple<http::Method, std::string, int>, const RouteDef*> shapes;
  for (auto& r : routes) {
    auto key = std::make_tuple(r.method, detail::Router::shape(r.path), r.rank);
    auto [it, inserted] = shapes.emplace(key, &r);
    if (!inserted)
      errors.push_back("route conflict: " + describe(*it->second) + " and " + describe(r) +
                       " match the same requests at rank " + std::to_string(r.rank) +
                       "; give one of them a different rank");
  }

  c.routes = std::move(routes);
  c.placement = std::make_unique<detail::Placement[]>(c.routes.size());

  // 5. Fairings get the last word. Shield is on unless one is attached already
  // (Shield::none() turns it off); attached last, its on_response runs first.
  if (!std::ranges::any_of(c.fairings, [](auto& f) { return dynamic_cast<detail::FairingModel<Shield>*>(f.get()); }))
    c.fairings.push_back(std::make_unique<detail::FairingModel<Shield>>(Shield{}));
  Ignite ig(c, errors);
  for (auto& f : c.fairings) {
    try {
      f->on_ignite(ig);
    } catch (const std::exception& e) {
      errors.push_back("fairing " + std::string(f->name()) + " threw during ignite: " + e.what());
    }
  }

  if (!errors.empty()) {
    c.routes.clear();
    return std::unexpected(IgniteError{std::move(errors)});
  }
  c.router = std::make_shared<detail::Router>(c.routes);
  c.ignited = true;
  log::detail::set_default(c.log);  // lines logged outside a request go here
  return {};
}

void Crocket::prepare(Request& req) const {
  if (req.request_id.empty()) {
    auto hdr = req.header("x-request-id");
    req.request_id = (hdr && valid_request_id(*hdr)) ? std::string(*hdr) : detail::generate_request_id();
  }
  if (req.trace_id.empty())
    if (auto tp = req.header("traceparent")) req.trace_id = trace_id_of(*tp);
  if (req.deadline.at == Clock::time_point::max()) {
    req.deadline.at = req.received + core_->config.request_timeout;
    // A gRPC client's deadline can only shorten the configured one.
    if (auto t = req.header("grpc-timeout"))
      if (auto d = parse_grpc_timeout(*t)) req.deadline.at = std::min(req.deadline.at, req.received + *d);
  }
  if (req.scheme.empty()) {  // once: the engine prepares before handle()
    req.remote_addr = req.peer_addr;
    req.scheme = req.tls ? "https" : "http";
    if (!core_->trusted_proxies.empty())
      detail::resolve_client(req, core_->trusted_proxies, core_->config.proxy_header);
  }
  req.state_registry = &core_->state;
  req.dev_profile = core_->config.profile == Profile::Dev;
  req.json_options = core_->config.json;
}

namespace detail {
namespace {

thread_local Exchange* t_exchange = nullptr;

/// Marks the request this thread runs, for current_exchange().
struct ExchangeScope {
  Exchange* prev;
  explicit ExchangeScope(Exchange* ex) : prev(std::exchange(t_exchange, ex)) {}
  ~ExchangeScope() { t_exchange = prev; }
};

/// Resumes tasks when no engine is running (LocalClient, tasks outside requests).
class FallbackPool final : public Executor {
 public:
  FallbackPool() {
    for (int i = 0; i < 4; ++i) threads_.emplace_back([this] { loop(); });
  }
  ~FallbackPool() override {
    {
      std::lock_guard lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void post(std::move_only_function<void()> job) override {
    {
      std::lock_guard lk(mu_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  void loop() {
    for (;;) {
      std::move_only_function<void()> job;
      {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::move_only_function<void()>> jobs_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
};

Executor& fallback_pool() {
  // Never destroyed: at exit, the sleep_for timer thread may still post to it,
  // and static destructors run in no order the two could agree on.
  static auto* pool = new FallbackPool;
  return *pool;
}

}  // namespace

Executor& fallback_executor() { return fallback_pool(); }

Exchange* current_exchange() noexcept { return t_exchange; }

LoopHold::~LoopHold() {
  auto held = std::chrono::steady_clock::now() - since;
  if (!report || held < kLoopHoldWarning) return;
  // Once per handler: the point is to find it, not to flood the log.
  static std::mutex mu;
  static std::set<std::string_view> warned;
  {
    std::lock_guard lk(mu);
    if (!warned.insert(handler).second) return;
  }
  log::warn("{blocking_handler} held its event loop for {ms} ms, stalling the other connections on it; an async "
            "handler must await, not block (a plain function runs on a worker and may block)",
            handler.empty() ? std::string_view("a fairing") : handler,
            std::chrono::duration_cast<std::chrono::milliseconds>(held).count());
}

void resume_on_worker(Exchange* ex, std::coroutine_handle<> h) {
  if (ex) ex->post(h);
  else fallback_pool().post([h] { h.resume(); });
}

void Exchange::post(std::coroutine_handle<> h) {
  Executor* ex = loop ? loop : app_.core_->executor.load();
  (ex ? *ex : fallback_pool()).post([this, h] {
    // Declared first, so it reports after the scopes end: resume() may finish
    // the request, and then nothing here may touch it.
    std::optional<LoopHold> hold;
    if (loop) hold.emplace(req.handler);
    ExchangeScope current{this};
    log::detail::RequestScope scope{app_.core_->log, &req};
    note_worker_deadline(req.deadline.at);
    h.resume();
  });
}

void Exchange::handler_done() {
  int expected = Running;
  if (state_.compare_exchange_strong(expected, FinishedEarly)) return;  // the pipeline completes it
  app_.complete(this);
}

void respond_exception(std::exception_ptr e, Request& rq, Response& rs) {
  rs = Response{};
  try {
    std::rethrow_exception(e);
  } catch (const ApiError& err) {
    write_error(err, rq, rs);
  } catch (const std::exception& err) {
    write_error(ApiError::internal(std::string("uncaught exception in ") + std::string(rq.handler) + ": " + err.what()),
                rq, rs);
  } catch (...) {
    write_error(ApiError::internal("uncaught non-std exception in " + std::string(rq.handler)), rq, rs);
  }
}

void async_handler_done(Request& rq) { rq.exchange->handler_done(); }

}  // namespace detail

void Crocket::handle_async(Request&& req, std::move_only_function<void(Response&&)> done, detail::Executor* loop) {
  run(new detail::Exchange(*this, std::move(req), std::move(done), loop));
}

bool Crocket::loop_route(http::Method method, std::string_view path) const {
  if (!core_->ignited) return false;
  auto candidates = core_->router->match(method, path);
  if (candidates.empty()) return true;  // a 404 or 405 crocket writes itself: nothing that can block
  for (const auto* c : candidates)
    if (!runs_on_loop(*c->def)) return false;
  return true;
}

bool Crocket::runs_on_loop(const RouteDef& def) const {
  if (def.builtin != detail::Builtin::None) return false;
  if (def.async) return true;
  if (!core_->config.adaptive_placement || def.may_block) return false;
  return core_->placement[std::size_t(&def - core_->routes.data())].on_loop.load(std::memory_order_relaxed);
}

void Crocket::ran_on_worker(const RouteDef& def, std::chrono::steady_clock::duration took) {
  using P = detail::Placement;
  auto& p = core_->placement[std::size_t(&def - core_->routes.data())];
  if (p.on_loop.load(std::memory_order_relaxed)) return;
  if (std::chrono::steady_clock::now().time_since_epoch().count() < p.retry_at.load(std::memory_order_relaxed)) return;
  if (took >= P::kFast) {
    if (p.fast_runs.load(std::memory_order_relaxed)) p.fast_runs.store(0, std::memory_order_relaxed);
    return;
  }
  if (p.fast_runs.fetch_add(1, std::memory_order_relaxed) + 1 != P::kPromoteAfter) return;
  p.on_loop.store(true, std::memory_order_relaxed);
  log::info("{function} runs on the event loops from now on: {runs} runs in a row each took under {limit_us} us",
            def.handler, P::kPromoteAfter, std::chrono::microseconds(P::kFast).count());
}

void Crocket::ran_on_loop(const RouteDef& def, std::chrono::steady_clock::duration took) {
  using P = detail::Placement;
  if (took <= P::kSlow) return;  // the common case: nothing to record
  auto& p = core_->placement[std::size_t(&def - core_->routes.data())];
  if (took <= P::kStall) {
    // Slow, but maybe only descheduled: a strike. Enough of them close together is blocking.
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto since = p.strikes_since.load(std::memory_order_relaxed);
    if (now - since > std::chrono::steady_clock::duration(P::kStrikeWindow).count() &&
        p.strikes_since.compare_exchange_strong(since, now, std::memory_order_relaxed)) {
      p.strikes.store(1, std::memory_order_relaxed);
      return;
    }
    if (p.strikes.fetch_add(1, std::memory_order_relaxed) + 1 < P::kStrikes) return;
  }
  if (!p.on_loop.exchange(false, std::memory_order_relaxed)) return;  // demoted already
  p.strikes.store(0, std::memory_order_relaxed);
  auto n = p.demotions.fetch_add(1, std::memory_order_relaxed) + 1;
  auto wait = P::kRetryAfter * (1u << std::min(n - 1, 8u));  // 10 s, 20 s, ... about 43 minutes
  p.fast_runs.store(0, std::memory_order_relaxed);
  p.retry_at.store((std::chrono::steady_clock::now() + wait).time_since_epoch().count(), std::memory_order_relaxed);
  log::warn("{function} blocked its event loop ({ms} ms, after {slow_runs} slow runs within {window_s} s), stalling "
            "the other connections on it, so it runs on workers again; it may move back after {retry_s} s of fast runs",
            def.handler, std::chrono::duration_cast<std::chrono::milliseconds>(took).count(),
            took > P::kStall ? 1u : P::kStrikes, std::chrono::duration_cast<std::chrono::seconds>(P::kStrikeWindow).count(),
            std::chrono::duration_cast<std::chrono::seconds>(wait).count());
}

Response Crocket::handle(Request req) {
  // Usually the response is back before handle_async returns: no waiting then.
  enum : int { Pending, Waiting, Delivered };
  std::atomic<int> state{Pending};
  std::binary_semaphore ready{0};
  Response out;
  handle_async(std::move(req), [&](Response&& res) {
    out = std::move(res);
    if (state.exchange(Delivered) == Waiting) ready.release();
  });
  if (state.exchange(Waiting) != Delivered) ready.acquire();
  return out;
}

void Crocket::run(detail::Exchange* ex) {
  Request& req = ex->req;
  Response& res = ex->res;
  req.exchange = ex;
  prepare(req);
  detail::ExchangeScope current{ex};
  log::detail::RequestScope scope{core_->log, &req};  // log lines from here on carry this request
  std::optional<detail::LoopHold> hold;
  if (ex->loop) hold.emplace();
  core_->stats.in_flight.fetch_add(1, std::memory_order_relaxed);

  bool finished = false;
  if (auto host = req.header("host").value_or("");
      !detail::host_allowed(host, core_->config.allowed_hosts, req.dev_profile)) {
    write_error({400, "host.invalid", "host not allowed", "Host: '" + std::string(host) + "'"}, req, res);
    finished = true;
  }
  for (auto& f : core_->fairings) {
    if (finished) break;
    try {
      if (auto early = f->on_request(req)) {
        res = std::move(*early);
        finished = true;
        break;
      }
    } catch (const std::exception& e) {
      write_error(ApiError::internal("fairing " + std::string(f->name()) + " on_request threw: " + e.what()), req, res);
      finished = true;
      break;
    }
  }
  bool suspended = !finished && run_routes(req, res);
  if (!suspended && !finished) warn_if_late(req);
  if (hold) {  // reported now, while the request is certainly alive
    hold->handler = req.handler;
    hold->report = !ex->loop_hold_reported;
    hold.reset();
  }
  if (ex->reroute) {  // after the last use of req here: the worker takes it over
    ex->reroute = false;
    detail::Executor* workers = core_->executor.load();
    (workers ? *workers : detail::fallback_executor()).post([this, ex] {
      detail::ExchangeScope current{ex};
      log::detail::RequestScope scope{core_->log, &ex->req};
      if (ex->req.deadline.expired()) {  // nobody would see what the handler did
        core_->stats.abandoned.fetch_add(1, std::memory_order_relaxed);
        write_error({504, "deadline.exceeded", "request deadline exceeded", {}}, ex->req, ex->res);
        ex->handler_done();
        return;
      }
      detail::note_worker_deadline(ex->req.deadline.at);
      if (run_routes(ex->req, ex->res)) return;  // its async handler finishes it
      warn_if_late(ex->req);
      ex->handler_done();
    });
  }
  if (suspended && ex->suspend()) return;  // the handler completes it
  complete(ex);
}

void Crocket::warn_if_late(const Request& req) const {
  auto now = Clock::now();
  if (now <= req.deadline.at || req.deadline.at == Clock::time_point::max()) return;
  // The client already has a 504: the handler's thread was busy for nothing.
  auto late = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.deadline.at).count();
  log::warn("the handler returned {late_ms} ms after its request's deadline; long work should stop once "
            "Deadline::expired() (or its stop_token) says so",
            std::int64_t(late));
}

void Crocket::complete(detail::Exchange* ex) {
  std::unique_ptr<detail::Exchange> owned(ex);
  finish(ex->req, ex->res);
  core_->stats.in_flight.fetch_sub(1, std::memory_order_relaxed);
  auto done = std::move(ex->done_);
  Response res = std::move(ex->res);
  owned.reset();  // the request goes before the response is delivered
  done(std::move(res));
}

Response Crocket::reject(Request& req, const ApiError& err) {
  prepare(req);
  log::detail::RequestScope scope{core_->log, &req};
  Response res;
  write_error(err, req, res);
  finish(req, res);
  return res;
}

void Crocket::finish(const Request& req, Response& res) {
  // A header built from untrusted input must not split the response.
  auto bad_header = [&]() -> std::optional<std::string> {
    for (auto* hs : {&res.headers, &res.trailers})
      for (auto& [k, v] : *hs)
        if (!http::valid_header_name(k) || !http::valid_header_value(v))
          return http::valid_header_name(k) ? "response header '" + k + "'" : std::string("a response header name");
    return std::nullopt;
  }();
  if (bad_header) {
    res = Response{};
    write_error(ApiError::internal(*bad_header + " contains a character not allowed in HTTP headers (CR, LF, NUL, ...)"),
                req, res);
  }
  res.headers.set("x-request-id", req.request_id);
  detail::conditional(req, res);
  auto& fs = core_->fairings;
  for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
    try {
      (*it)->on_response(req, res);
    } catch (const std::exception& e) {
      log::error("fairing {fairing} on_response threw: {error}", (*it)->name(), std::string_view(e.what()));
    }
  }
}

bool Crocket::run_routes(Request& req, Response& res) {
  if (!core_->ignited) {
    write_error(ApiError::internal("request handled before ignite"), req, res);
    return false;
  }
  auto candidates = core_->router->match(req.method, req.path);
  bool forwarded = false;
  std::string forward_detail;
  for (const auto* cand : candidates) {
    const RouteDef& def = *cand->def;
    if (req.exchange && req.exchange->loop && !runs_on_loop(def)) {
      // On an event loop, but this handler may block (a fairing rewrote the
      // request, or an async candidate forwarded): run() routes it again on a worker.
      req.exchange->loop = nullptr;
      req.exchange->reroute = true;
      return true;
    }
    req.route_template = def.path;
    req.route = &def;
    req.handler = def.handler;
    req.captures = candidates.captures(*cand);
    if (def.builtin != detail::Builtin::None) {
      res = builtin(def.builtin, req);
      return false;
    }

    res = Response{};
    try {
      // A plain function is timed, to learn where it should run (adaptive placement).
      bool timed = core_->config.adaptive_placement && !def.async && !def.may_block;
      auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      auto out = def.invoke(req, res);
      if (timed && out.kind != detail::Outcome::Forward) {
        auto took = std::chrono::steady_clock::now() - started;
        if (req.exchange && req.exchange->loop) {
          req.exchange->loop_hold_reported = true;  // ran_on_loop reports a slow run
          ran_on_loop(def, took);
        } else {
          ran_on_worker(def, took);
        }
      }
      if (out.kind == detail::Outcome::Async) return true;
      if (out.kind == detail::Outcome::Done) return false;
      if (out.kind == detail::Outcome::Forward) {
        forwarded = true;
        forward_detail = std::string(req.handler) + ": " + out.detail;
        continue;
      }
      res = Response{};
      write_error(out.error, req, res);
      res.failure_kind = out.failure_kind;
      return false;
    } catch (...) {
      detail::respond_exception(std::current_exception(), req, res);
      return false;
    }
  }

  res = Response{};
  if (forwarded) {
    write_error(ApiError{404, "path.invalid", "not found", forward_detail}, req, res);
    res.failure_kind = "path";
    return false;
  }
  req.route_template = {};
  req.route = nullptr;
  req.handler = {};
  if (grpc::is_grpc_request(req)) {
    write_error(grpc::error(grpc::Code::Unimplemented, "grpc.unimplemented", "unknown method " + req.path), req, res);
    return false;
  }
  auto allowed = core_->router->allowed(req.path);
  if (!allowed.empty()) {
    std::string allow;
    for (auto m : allowed) {
      if (!allow.empty()) allow += ", ";
      allow += http::method_name(m);
    }
    write_error(ApiError{405, "method.not_allowed", "method not allowed", {}}, req, res);
    res.headers.set("allow", allow);
    return false;
  }
  write_error(ApiError::not_found(), req, res);
  return false;
}

Response Crocket::builtin(detail::Builtin b, Request& /*req*/) {
  Response res;
  res.set_content_type("application/json");
  res.headers.set("cache-control", "no-store");
  switch (b) {
    case detail::Builtin::Healthz:
      res.body = R"({"status":"ok"})";
      break;
    case detail::Builtin::Readyz: {
      if (core_->stats.draining.load()) {
        res.status = 503;
        res.error_code = "server.draining";
        res.body = R"({"status":"draining"})";
        break;
      }
      bool ok = true;
      std::string checks;
      for (auto& rc : core_->ready_checks) {
        bool pass = false;
        try {
          pass = rc.check(core_->state.find(rc.key));
        } catch (...) {
        }
        ok = ok && pass;
        if (!checks.empty()) checks += ',';
        json::write_string(checks, rc.name);
        checks += pass ? R"(:"ok")" : R"(:"fail")";
      }
      res.status = ok ? 200 : 503;
      if (!ok) res.error_code = "not_ready";
      res.body = std::string(R"({"status":)") + (ok ? R"("ready")" : R"("not_ready")") + R"(,"checks":{)" + checks + "}}";
      break;
    }
    case detail::Builtin::Routes: {
      std::string& o = res.body;
      o = "[";
      bool first = true;
      for (auto& r : core_->routes) {
        if (!first) o += ',';
        first = false;
        o += R"({"method":)";
        json::write_string(o, http::method_name(r.method));
        o += R"(,"path":)";
        json::write_string(o, r.path);
        o += R"(,"handler":)";
        json::write_string(o, r.handler);
        o += R"(,"rank":)" + std::to_string(r.rank);
        o += R"(,"runs_on":)";
        o += runs_on_loop(r) ? R"("event loop")" : R"("workers")";
        o += R"(,"params":[)";
        for (std::size_t i = 0; i < r.params.size(); ++i) {
          if (i) o += ',';
          o += R"({"name":)";
          json::write_string(o, r.params[i].name);
          o += R"(,"source":)";
          json::write_string(o, r.params[i].source);
          o += R"(,"type":)";
          json::write_string(o, r.params[i].type);
          o += '}';
        }
        o += "]}";
      }
      o += ']';
      break;
    }
    case detail::Builtin::None:
      break;
  }
  return res;
}

int Crocket::launch(LaunchOptions opts) {
  auto ig = ignite();
  std::vector<std::string> problems = ig ? std::vector<std::string>{} : ig.error().problems;
  bool tls = !opts.tls_cert.empty() || !opts.tls_key.empty();
  if (tls) {
    if (opts.tls_cert.empty() || opts.tls_key.empty())
      problems.push_back("TLS needs both tls_cert and tls_key");
    for (auto* f : {&opts.tls_cert, &opts.tls_key})
      if (!f->empty() && !std::ifstream(*f).good()) problems.push_back("TLS file not readable: " + *f);
  }
  if (!problems.empty()) {
    std::fprintf(stderr, "%s\n", IgniteError{problems}.message().c_str());
    return 1;
  }
  bool dev = core_->config.profile == Profile::Dev;
  if (opts.host.empty()) opts.host = dev ? "127.0.0.1" : "0.0.0.0";
  if (dev) print_banner(core_->routes);
  return detail::run_engine(*this, opts);
}

namespace detail {
void shut_down(Crocket& app) {
  auto& fs = app.core().fairings;
  for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
    try {
      (*it)->on_shutdown();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "crocket: fairing on_shutdown threw: %s\n", e.what());
    }
  }
  app.core().state.clear();
  app.flush_logs();
}
}  // namespace detail

// ---- LocalClient -------------------------------------------------------------

LocalClient::LocalClient(Crocket& app) : app_(app) {
  if (auto r = app_.ignite(); !r) throw std::runtime_error(r.error().message());
}

LocalClient::Call LocalClient::grpc(std::string_view path, std::string_view message) {
  Call c{app_, http::Method::Post, path};
  c.header("content-type", "application/grpc").header("te", "trailers").body(grpc::frame(message));
  return c;
}

LocalClient::Call::Call(Crocket& app, http::Method m, std::string_view target) : app_(app) {
  req_.method = m;
  req_.method_text = http::method_name(m);
  auto q = target.find('?');
  req_.path = detail::percent_decode(target.substr(0, q), false);
  if (q != std::string_view::npos) req_.query = detail::parse_query(target.substr(q + 1));
  req_.received = Clock::now();
}

}  // namespace crocket
