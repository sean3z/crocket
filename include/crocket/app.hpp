#pragma once
// Crocket, the application: managed state, fairings, mounted routes, ignite, launch.

#include "crocket/detail/invoke.hpp"
#include "crocket/http.hpp"
#include "crocket/log.hpp"
#include "crocket/request.hpp"
#include "crocket/state.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace crocket {

/// Release is the default. Dev is for a developer's machine: readable logs, a
/// route banner, and error bodies that include ApiError::detail.
enum class Profile : std::uint8_t { Release, Dev };

/// Which header trusted proxies use to report the client.
enum class ProxyHeader : std::uint8_t {
  XForwardedFor,  // X-Forwarded-For and X-Forwarded-Proto (nginx, AWS ALB, Envoy, ...)
  Forwarded,      // RFC 7239 Forwarded
};

/// Where and how the server listens. TLS is enabled when both cert and key are set.
struct ServerConfig {
  /// "host:port", ":port", "port" or "[v6]:port". When set it replaces host and port.
  std::string listen = {};
  std::string host = {};  // empty: 0.0.0.0, or 127.0.0.1 in the dev profile
  std::uint16_t port = 8000;
  /// Handler threads. 0 (the default) picks max(4, cores), raised to the size of the
  /// largest managed blocking pool (db::Pool, Pool<T>) so every connection can be in use
  /// at once. Any nonzero value (code, CROCKET_WORKERS or launch({.workers})) is used as is.
  unsigned workers = 0;
  unsigned event_loops = 0;  // network I/O threads; 0 = one per physical core
  std::string tls_cert = {};
  std::string tls_key = {};
  bool http2 = true;  // offer h2 via ALPN when TLS is on
  /// Without TLS, also accept HTTP/2 with prior knowledge (and Upgrade: h2c), as
  /// gRPC clients use on an insecure channel. HTTP/1.1 keeps working on the port.
  bool h2_prior_knowledge = false;
};

/// Size and concurrency limits.
struct LimitsConfig {
  std::size_t max_body_bytes = 1 << 20;  // 413 body.too_large
  /// Request bodies held in memory at once, every request together: past it,
  /// 503 server.busy. With max_body_bytes, it bounds what bodies can take
  /// however many clients send them.
  std::size_t max_body_memory = std::size_t(256) << 20;
  std::size_t max_in_flight = 1024;     // 503 server.busy
  std::size_t max_header_bytes = 8192;  // names + values; 431 headers.too_large
  std::size_t max_header_count = 100;   // 431 headers.too_large
  /// Open connections, all event loops together. Past it, new connections wait
  /// in the kernel's accept queue until one closes, as in h2o (whose default
  /// this is); they are not refused.
  unsigned max_connections = 1024;
  /// Open connections from one client address. Past it, a new one is closed at
  /// once. 0, the default, is no limit: behind a load balancer every connection
  /// comes from the balancer's address. Set it when clients connect directly.
  unsigned max_connections_per_ip = 0;
  /// Concurrent requests (streams) on one HTTP/2 connection (h2o's default).
  unsigned http2_max_streams = 100;
  /// A 200 to GET or HEAD without an ETag gets one, a hash of the body, when
  /// the body is at most this long (hashing costs about a microsecond per 10
  /// KiB). 0: never. Set an ETag yourself for larger bodies.
  std::size_t etag_max_bytes = std::size_t(1) << 20;
};

struct TimeoutsConfig {
  std::chrono::milliseconds request{30'000};  // 504 deadline.exceeded
  std::chrono::milliseconds drain{10'000};    // graceful shutdown budget
};

/// Who is in front of the server, and which host names it answers to.
struct ProxyConfig {
  /// Proxies in front of the server, as addresses or CIDR ranges ("10.0.0.0/8",
  /// "fd00::/8"). Requests from them have Request::remote_addr and scheme taken
  /// from `header`; that header is ignored from everyone else.
  std::vector<std::string> trusted = {};
  ProxyHeader header = ProxyHeader::XForwardedFor;
  /// Host names this server answers to ("api.example.com", "*.example.com").
  /// Any other Host is 400 host.invalid. Empty allows every host; the dev
  /// profile also allows localhost.
  std::vector<std::string> allowed_hosts = {};
};

/// JWT / OIDC settings for the optional crocket::auth component.
/// Core crocket never reads them; auth::Jwt::from(cfg.auth) does.
/// mTLS client certificates are not part of this (a later change).
struct AuthConfig {
  std::string issuer;     // CROCKET_AUTH_ISSUER; exact `iss` match
  std::string audience;   // CROCKET_AUTH_AUDIENCE; empty skips the aud check
  std::string jwks;       // CROCKET_AUTH_JWKS; empty: discover from the issuer
  /// Shared secret for HMAC. Empty rejects every HS* algorithm. `Jwt::from`
  /// adds HS256 to `.algorithms` when this is set; the struct's default list
  /// does not include it.
  std::string secret;  // CROCKET_AUTH_SECRET
  std::chrono::milliseconds leeway{60'000};
  std::string roles_claim = "roles";
  std::chrono::milliseconds jwks_refresh{600'000};
};

/// The database crocket::db connects to (the optional crocket::db component).
/// Core crocket never reads it; db::Pool(cfg.database) does.
struct DatabaseConfig {
  std::string url = {};  // "postgresql://user:pass@host/db"; empty: libpq's PG* variables
  unsigned pool_size = 8;  // connections, opened at startup
  /// Longest a handler waits for a free connection (bounded by the request's
  /// deadline too); past it, 503 db.busy.
  std::chrono::milliseconds pool_wait{5'000};
};

/// Everything crocket can be told, in one place. Precedence, lowest first:
/// these defaults (or from_env's base), the profile (dev()), the environment (from_env()), then
/// whatever the program sets on the result, then launch({...}).
struct Config {
  Profile profile = Profile::Release;
  ServerConfig server = {};
  LimitsConfig limits = {};
  TimeoutsConfig timeouts = {};
  ProxyConfig proxy = {};
  AuthConfig auth = {};
  DatabaseConfig database = {};
  bool debug_routes = false;  // expose GET /__routes
  json::ReadOptions json = {};  // limits for Json<T> bodies
  /// Run a plain-function route on the event loops once it has proved fast
  /// (1,000 runs in a row under 100 us), and back on workers when it turns out
  /// to block there (one run over 50 ms, or 8 over 1 ms within a second). Off:
  /// plain functions always run on workers.
  bool adaptive_placement = true;
  log::Options log = {};  // level, sampling, redaction, sink

  /// Dev defaults: GET /__routes, a one-hour deadline (room for a debugger
  /// breakpoint), a one-second drain, debug logs and listening on 127.0.0.1.
  static Config dev();
  /// The profile from CROCKET_PROFILE (dev or release), then every other
  /// CROCKET_* variable on top (see the README's table). Throws
  /// std::invalid_argument naming the variable when a value does not parse.
  /// `base` replaces the built-in defaults, for a program whose own defaults
  /// differ (a gRPC server on 50051) but should still yield to the environment.
  static Config from_env();
  static Config from_env(Config base);
};

/// Server settings for launch({...}), on top of Config::server. Unset fields
/// keep the Config's value: `.launch({.listen = "8080"})`.
struct ServerOverrides {
  std::string listen = {};  // "host:port", ":port" or "port"
  std::optional<std::string> host = {};
  std::optional<std::uint16_t> port = {};
  std::optional<unsigned> workers = {};
  std::optional<unsigned> event_loops = {};
  std::optional<std::string> tls_cert = {};
  std::optional<std::string> tls_key = {};
  std::optional<bool> http2 = {};
  std::optional<bool> h2_prior_knowledge = {};
};

namespace detail {
/// "8080", ":8080", "0.0.0.0:8080", "[::1]:8080" into host and port. Throws
/// std::invalid_argument for anything else.
void parse_listen(std::string_view listen, std::string& host, std::uint16_t& port);
/// cfg.server with the overrides applied and `listen` resolved.
ServerConfig resolve_server(const Config& cfg, const ServerOverrides& o);
/// The worker count launch uses: `configured` when nonzero (Config, CROCKET_WORKERS or
/// launch({.workers})), else max(4, cores), raised to the largest managed blocking
/// pool (capped at 1024) so each pooled connection can be in use at once.
unsigned resolve_workers(unsigned configured, unsigned cores, std::size_t blocking_pool_size);
}

/// What a mount serves. Http mounts take a path prefix ("/api"); Grpc mounts
/// take a protobuf package ("helloworld", or "" for none).
enum class Mode : std::uint8_t { Http, Grpc };

/// One handler parameter as shown by /__routes.
struct ParamInfo {
  std::string_view name;
  std::string_view source;  // "path" or the extractor kind
  std::string_view type;
};

namespace detail {
enum class Builtin : std::uint8_t { None, Healthz, Readyz, Routes };
namespace openapi {
struct Operation;
}
}

/// A route, as produced by reflect_routes() and adjusted by mount().
struct RouteDef {
  http::Method method = http::Method::Get;
  std::string path;  // template, including the mount prefix
  int rank = 0;
  std::string_view handler;  // "api::create"
  std::vector<ParamInfo> params;
  std::vector<detail::StateDep> needs;
  detail::Invoker invoke = nullptr;
  detail::Builtin builtin = detail::Builtin::None;
  bool async = false;  // the handler returns Task<T> (or another awaitable): it runs on the event loop
  bool may_block = false;  // takes an extractor that may wait (State<Pool<T>>): always on workers
  Mode mode = Mode::Http;
  std::string_view service;  // gRPC: "Greeter"
  std::string_view rpc;      // gRPC: "SayHello"
  void (*openapi)(detail::openapi::Operation&) = nullptr;  // describes it for OpenApi (HTTP routes)
};

using Routes = std::vector<RouteDef>;

struct IgniteError {
  std::vector<std::string> problems;
  [[nodiscard]] std::string message() const;
};

class Crocket;
class Ignite;

namespace detail {

struct FairingBase {
  virtual ~FairingBase() = default;
  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual void on_ignite(Ignite&) {}
  virtual std::optional<Response> on_request(Request&) { return std::nullopt; }
  virtual void on_response(const Request&, Response&) {}
  virtual void on_shutdown() {}
};

template <class F>
class FairingModel final : public FairingBase {
 public:
  explicit FairingModel(F f) : f_(std::move(f)) {}
  [[nodiscard]] std::string_view name() const override { return type_display_name<F>(); }
  void on_ignite(Ignite& ig) override {
    if constexpr (requires { f_.on_ignite(ig); }) f_.on_ignite(ig);
  }
  std::optional<Response> on_request(Request& rq) override {
    if constexpr (requires { { f_.on_request(rq) } -> std::same_as<std::optional<Response>>; })
      return f_.on_request(rq);
    else if constexpr (requires { f_.on_request(rq); }) {
      f_.on_request(rq);
      return std::nullopt;
    } else
      return std::nullopt;
  }
  void on_response(const Request& rq, Response& rs) override {
    if constexpr (requires { f_.on_response(rq, rs); }) f_.on_response(rq, rs);
  }
  void on_shutdown() override {
    if constexpr (requires { f_.on_shutdown(); }) f_.on_shutdown();
  }

 private:
  F f_;
};

class Router;

struct ReadyCheck {
  std::string_view name;
  TypeKey key;
  bool (*check)(void*);
};

/// Runs jobs on worker threads: the engine's pool while launched.
struct Executor {
  virtual ~Executor() = default;
  virtual void post(std::move_only_function<void()> job) = 0;
};

/// One request in flight. Owns the Request and Response at a fixed address
/// while the handler, possibly suspended across threads, refers to them, and
/// delivers the Response once.
class Exchange {
 public:
  using Done = std::move_only_function<void(Response&&)>;
  Exchange(Crocket& app, Request&& rq, Done done, Executor* loop)
      : req(std::move(rq)), loop(loop), app_(app), done_(std::move(done)) {}

  Request req;
  Response res;
  /// The event loop running this request (async handlers stay on it), or null
  /// on a worker or in LocalClient.
  Executor* loop;
  bool reroute = false;  // routing found a handler that may block: route again on a worker
  bool loop_hold_reported = false;  // a promoted plain function ran here: its demotion says it all

  /// The async handler finished filling `res`. Completes the request, unless
  /// the pipeline has not yet seen it suspend (it completes it then).
  void handler_done();
  /// Resumes `h` with this request's context: on its event loop, else on a worker.
  void post(std::coroutine_handle<> h);

 private:
  friend class crocket::Crocket;
  enum : int { Running, Suspended, FinishedEarly };
  /// After the handler suspended: false if it has finished meanwhile.
  bool suspend() {
    int expected = Running;
    return state_.compare_exchange_strong(expected, Suspended);
  }
  Crocket& app_;
  Done done_;
  std::atomic<int> state_{Running};
};

/// Workers when no engine runs (LocalClient, tasks outside requests).
Executor& fallback_executor();

/// Where a plain-function route runs, learned from how long it takes
/// (Config::adaptive_placement). One per route, built at ignite.
struct Placement {
  static constexpr std::uint32_t kPromoteAfter = 1000;               // fast runs in a row on workers
  static constexpr std::chrono::microseconds kFast{100};              // ... each under this
  // On a loop, a run's wall time also counts time the thread was not on a CPU
  // (the OS ran something else), so one slow run is not proof of blocking:
  static constexpr std::chrono::milliseconds kStall{50};      // one run over this: demoted at once
  static constexpr std::chrono::milliseconds kSlow{1};        // kStrikes runs over this ...
  static constexpr std::uint32_t kStrikes = 8;
  static constexpr std::chrono::seconds kStrikeWindow{1};     // ... within this: demoted
  static constexpr std::chrono::seconds kRetryAfter{10};      // doubled per demotion
  std::atomic<bool> on_loop{false};
  std::atomic<std::uint32_t> strikes{0};
  std::atomic<std::int64_t> strikes_since{0};  // steady_clock ticks: start of the strike window
  std::atomic<std::uint32_t> fast_runs{0};
  std::atomic<std::uint32_t> demotions{0};
  std::atomic<std::int64_t> retry_at{0};  // steady_clock ticks; no promotion before
};

/// Measures how long a request holds its event loop; warns (once per handler)
/// past kLoopHoldWarning, since every connection on the loop waits meanwhile.
inline constexpr std::chrono::milliseconds kLoopHoldWarning{10};
struct LoopHold {
  std::string_view handler;  // static storage (RouteDef); may be set after construction
  bool report = true;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  explicit LoopHold(std::string_view h = {}) : handler(h) {}
  ~LoopHold();
};

/// A latency histogram with fixed bounds (Prometheus buckets), in steady_clock
/// nanoseconds. record() is for one writer per instance (a plain load and
/// store); record_shared() for several (a locked add).
struct LatencyHistogram {
  static constexpr std::array<std::int64_t, 14> kBoundsNs = {
      10'000,     25'000,     50'000,     100'000,     250'000,     500'000,     1'000'000,
      2'500'000,  5'000'000,  10'000'000, 25'000'000,  50'000'000,  100'000'000, 1'000'000'000};
  std::array<std::atomic<std::uint64_t>, kBoundsNs.size()> buckets{};  // not cumulative; +Inf is count
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::int64_t> sum_ns{0};

  static std::size_t bucket(std::int64_t ns) {
    std::size_t i = 0;
    while (i < kBoundsNs.size() && ns > kBoundsNs[i]) ++i;
    return i;
  }
  void record(std::int64_t ns) {
    if (ns < 0) ns = 0;
    if (auto i = bucket(ns); i < kBoundsNs.size()) buckets[i].store(buckets[i].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    sum_ns.store(sum_ns.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
    count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_release);  // last: see Metrics
  }
  void record_shared(std::int64_t ns) {
    if (ns < 0) ns = 0;
    if (auto i = bucket(ns); i < kBoundsNs.size()) buckets[i].fetch_add(1, std::memory_order_relaxed);
    sum_ns.fetch_add(ns, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_release);
  }
};

/// What one worker thread records about itself; only that worker writes it.
struct alignas(64) WorkerUsage {
  std::atomic<std::int64_t> busy_ns{0};   // time spent running jobs, finished ones
  std::atomic<std::int64_t> since_ns{0};  // steady_clock ns the current job started; 0 when idle
  LatencyHistogram queue_wait;            // queued -> picked up by this worker
};

/// Counters the framework maintains itself (read by Metrics).
struct Stats {
  std::atomic<std::int64_t> in_flight{0};
  std::atomic<bool> draining{false};
  std::atomic<std::uint64_t> log_dropped{0};
  /// Requests whose handler never ran: the deadline passed, or the client went
  /// away, while they waited for a worker.
  std::atomic<std::uint64_t> abandoned{0};
  /// The engine's workers while launched (0 otherwise), and the deadline of
  /// each one's current request (steady_clock nanoseconds; 0 when idle).
  std::atomic<unsigned> workers{0};
  std::unique_ptr<std::atomic<std::int64_t>[]> worker_deadline;
  std::unique_ptr<WorkerUsage[]> worker_usage;  // the same slots: busy time and queue wait
  /// Requests answered 503 server.busy before a handler ran: at max_in_flight,
  /// or with max_body_memory of bodies held.
  std::atomic<std::uint64_t> shed_in_flight{0};
  std::atomic<std::uint64_t> shed_body_memory{0};
  /// How late each event loop's 100 ms tick fires (every loop records here).
  LatencyHistogram loop_lag;
  std::atomic<std::int64_t> connections{0};             // open now
  std::atomic<std::uint64_t> connections_per_ip_closed{0};  // closed by max_connections_per_ip
  std::atomic<std::int64_t> body_bytes{0};              // request bodies held now (max_body_memory)
};

/// An address range from ProxyConfig::trusted (IPv4 as IPv4-mapped IPv6).
struct IpRange {
  std::array<std::uint8_t, 16> addr{};
  int prefix = 0;
};

struct Core {
  Config config;
  std::vector<IpRange> trusted_proxies;  // parsed at ignite
  std::shared_ptr<log::detail::Hub> log;  // where this app's log lines go
  std::atomic<Executor*> executor{nullptr};  // the engine's workers while launched
  std::unique_ptr<Placement[]> placement;    // per route (Config::adaptive_placement)
  StateRegistry state;
  std::vector<std::unique_ptr<FairingBase>> fairings;
  std::vector<ReadyCheck> ready_checks;
  /// Largest blocking pool managed (db::Pool, Pool<T>): the floor for the default worker count.
  std::size_t blocking_pool_size = 0;
  struct Mount {
    std::string base;
    Routes routes;
    Mode mode;
  };
  std::vector<Mount> mounts;
  std::vector<std::string> build_errors;  // reported at ignite
  Routes routes;                          // final table, built at ignite
  std::shared_ptr<Router> router;         // built at ignite
  Stats stats;
  bool ignited = false;
};

}  // namespace detail

/// Handed to Fairing::on_ignite. Call fail() to abort launch.
class Ignite {
 public:
  void fail(std::string reason) { errors_.push_back(std::move(reason)); }
  template <class T>
  [[nodiscard]] T* state() const { return core_.state.get<T>(); }
  [[nodiscard]] const Config& config() const { return core_.config; }
  [[nodiscard]] std::span<const RouteDef> routes() const { return core_.routes; }
  [[nodiscard]] const detail::Stats& stats() const { return core_.stats; }
  [[nodiscard]] const std::shared_ptr<log::detail::Hub>& log_hub() const { return core_.log; }

 private:
  friend class Crocket;
  Ignite(detail::Core& c, std::vector<std::string>& e) : core_(c), errors_(e) {}
  detail::Core& core_;
  std::vector<std::string>& errors_;
};

class Crocket {
 public:
  explicit Crocket(Config cfg = {});
  Crocket(Crocket&&) noexcept;
  Crocket& operator=(Crocket&&) noexcept;
  ~Crocket();

  /// Store exactly one T. A second manage<T>() is an ignite error.
  /// If T has `bool ready() const`, GET /readyz calls it.
  template <class T>
  Crocket& manage(T value) & {
    using Raw = std::remove_cvref_t<T>;
    using Stored = detail::manage_as_t<Raw>;
    auto name = detail::type_display_name<Raw>();
    // A type that is managed as another (auth::Jwt as Authenticator) is stored
    // under both keys, so the existing Auth extractor and the new type's ready()
    // both see it. Managing either one twice, or both, fails at ignite.
    if constexpr (!std::is_same_v<Stored, Raw>) {
      auto stored_name = detail::type_display_name<Stored>();
      if (core_->state.template get<Raw>())
        core_->build_errors.push_back(std::string("manage<") + name + ">() called twice");
      else if (core_->state.template get<Stored>())
        core_->build_errors.push_back(std::string("manage<") + name + ">() conflicts with an already managed " +
                                      stored_name);
      else {
        core_->state.template put<Stored>(Stored(value));
        core_->state.template put<Raw>(std::move(value));
        if constexpr (requires(const Raw& t) { { t.ready() } -> std::convertible_to<bool>; })
          core_->ready_checks.push_back({name, detail::type_key<Raw>(),
                                         [](void* p) -> bool { return static_cast<const Raw*>(p)->ready(); }});
      }
      return *this;
    }
    if (!core_->state.put<Raw>(std::move(value)))
      core_->build_errors.push_back(std::string("manage<") + name + ">() called twice");
    else {
      if constexpr (requires(const T& t) { { t.ready() } -> std::convertible_to<bool>; })
        core_->ready_checks.push_back({name, detail::type_key<T>(),
                                       [](void* p) -> bool { return static_cast<const T*>(p)->ready(); }});
      // A pool whose handlers block a worker for each connection in use: size the
      // default worker count so the whole pool can be busy at once.
      if constexpr (requires(const T& t) { requires T::crocket_may_block; { t.size() } -> std::convertible_to<std::size_t>; })
        if (auto* v = core_->state.template get<T>())
          core_->blocking_pool_size = std::max(core_->blocking_pool_size, std::size_t(v->size()));
    }
    return *this;
  }
  template <class T>
  Crocket&& manage(T value) && { return std::move(manage(std::move(value))); }

  /// Attach a fairing. on_request runs in attach order, on_response in
  /// reverse. Fairings run on worker threads concurrently: keep them short,
  /// synchronous and thread-safe.
  template <class F>
  Crocket& attach(F fairing) & {
    core_->fairings.push_back(std::make_unique<detail::FairingModel<F>>(std::move(fairing)));
    return *this;
  }
  template <class F>
  Crocket&& attach(F fairing) && { return std::move(attach(std::move(fairing))); }

  /// Serve `routes` under `base`. Mode::Grpc serves [[= grpc::rpc]] methods
  /// with `base` as the protobuf package: POST /<package>.<Service>/<Method>.
  Crocket& mount(std::string_view base, Routes routes, Mode mode = Mode::Http) &;
  Crocket&& mount(std::string_view base, Routes routes, Mode mode = Mode::Http) && {
    return std::move(mount(base, std::move(routes), mode));
  }

  Crocket& configure(Config cfg) &;
  Crocket&& configure(Config cfg) && { return std::move(configure(std::move(cfg))); }

  /// Validate everything and freeze the route table. Idempotent.
  std::expected<void, IgniteError> ignite();

  /// Ignite, serve until SIGINT/SIGTERM, drain, shut down. Returns an exit
  /// status (non-zero when ignite or launch fails).
  int launch(ServerOverrides overrides = {});

  /// The full request pipeline: request id, fairings, routing, extractors,
  /// handler, responder, error mapping. `done` gets the response exactly once:
  /// before handle_async returns, or later, when an async handler finishes.
  /// Called on an event loop (`loop`), async handlers run and resume there, and
  /// a synchronous handler moves to a worker. Used by the engine.
  void handle_async(Request&& req, std::move_only_function<void(Response&&)> done, detail::Executor* loop = nullptr);
  /// Whether every route that can answer method+path runs on the event loop
  /// (an async handler, or a plain function adaptive placement has promoted),
  /// or none does (crocket answers 404 or 405), so the loop can run the request itself.
  [[nodiscard]] bool loop_route(http::Method method, std::string_view path) const;
  /// handle_async, waiting for the response. Used by LocalClient.
  Response handle(Request req);

  /// Build an error response for a request rejected before routing
  /// (limits, timeouts). Runs on_response fairings so logs/metrics see it.
  Response reject(Request& req, const ApiError& err);

  /// Returns once every log line so far has been written (the writer is asynchronous).
  void flush_logs() const;

  /// Assign request id, deadline and state pointer. Idempotent.
  void prepare(Request& req) const;

  [[nodiscard]] const Config& config() const { return core_->config; }
  [[nodiscard]] std::span<const RouteDef> routes() const { return core_->routes; }
  [[nodiscard]] detail::Core& core() { return *core_; }

 private:
  friend class detail::Exchange;
  void run(detail::Exchange* ex);       // the pipeline; completes ex unless its handler suspended
  void complete(detail::Exchange* ex);  // finish, deliver and free
  void warn_if_late(const Request& req) const;  // logs a handler that outlived its deadline
  /// Routes req; true if the handler suspended (it completes the exchange later).
  bool run_routes(Request& req, Response& res);
  void label_route(Request& req) const;  // sets route/route_template from the router, for answers given before routing
  [[nodiscard]] bool runs_on_loop(const RouteDef& def) const;
  void ran_on_worker(const RouteDef& def, std::chrono::steady_clock::duration took);
  void ran_on_loop(const RouteDef& def, std::chrono::steady_clock::duration took);
  Response builtin(detail::Builtin b, Request& req);
  void finish(const Request& req, Response& res);
  std::unique_ptr<detail::Core> core_;
};

/// Start a Crocket: `crocket::build(cfg).attach(...).mount(...).launch({...})`.
[[nodiscard]] inline Crocket build(Config cfg = {}) { return Crocket{std::move(cfg)}; }

/// In-process client for tests: same pipeline as the network engine,
/// no sockets. Construction ignites the app and throws on ignite failure.
class LocalClient {
 public:
  class Call {
   public:
    Call& header(std::string_view k, std::string_view v) { req_.headers.add(k, v); return *this; }
    Call& body(std::string b) { req_.body = std::move(b); return *this; }
    Call& json(std::string b) {
      req_.headers.set("content-type", "application/json");
      req_.body = std::move(b);
      return *this;
    }
    /// The socket peer's address, e.g. a proxy's ("10.0.0.5").
    Call& remote(std::string_view addr) { req_.peer_addr = addr; return *this; }
    Call& bearer(std::string_view token) { return header("authorization", std::string("Bearer ") + std::string(token)); }
    /// Handles the request, then waits until its log lines are written, so a
    /// test can read them.
    Response dispatch() {
      if (req_.path.empty()) {  // a NUL in the path: the engine's 400
        auto res = app_.reject(req_, {400, "http.invalid", "invalid request path", {}});
        app_.flush_logs();
        return res;
      }
      auto res = app_.handle(std::move(req_));
      app_.flush_logs();
      return res;
    }

   private:
    friend class LocalClient;
    Call(Crocket& app, http::Method m, std::string_view target);
    Crocket& app_;
    Request req_;
  };

  explicit LocalClient(Crocket& app);
  Call get(std::string_view target) { return {app_, http::Method::Get, target}; }
  Call head(std::string_view target) { return {app_, http::Method::Head, target}; }
  Call post(std::string_view target) { return {app_, http::Method::Post, target}; }
  Call put(std::string_view target) { return {app_, http::Method::Put, target}; }
  Call patch(std::string_view target) { return {app_, http::Method::Patch, target}; }
  Call del(std::string_view target) { return {app_, http::Method::Delete, target}; }
  Call options(std::string_view target) { return {app_, http::Method::Options, target}; }
  /// A unary gRPC call: `client.grpc("/helloworld.Greeter/SayHello", proto::encode(req))`.
  Call grpc(std::string_view path, std::string_view message);

 private:
  Crocket& app_;
};

namespace detail {
std::string percent_decode(std::string_view s, bool plus_is_space);
/// The path a request is routed on, from the target's path (before any '?'):
/// split on '/', each segment percent-decoded, "." and ".." segments removed
/// (RFC 3986 5.2.4), and a '%' or '/' inside a segment written %25 or %2F, so
/// the result splits into exactly those segments ("/files/a%2Fb" stays two).
/// nullopt when a segment decodes to a NUL. The engine and LocalClient both
/// use it, so in-process tests route as production does.
std::optional<std::string> normalize_path(std::string_view raw);
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view q);
std::string generate_request_id();
/// On a worker thread: the deadline of the request it now runs (for
/// crocket_workers_stuck). Elsewhere, nothing.
void note_worker_deadline(Clock::time_point at);
/// On a worker thread: a job queued at `queued` started now (for
/// crocket_request_queue_wait_seconds). Elsewhere, nothing.
void note_queue_wait(Clock::time_point queued);
std::string_view reason_phrase(int status);
std::string iso8601_now();
/// iso8601_now() appended to `out`, without a temporary (log lines use it).
void append_iso8601_now(std::string& out);
}  // namespace detail

}  // namespace crocket
