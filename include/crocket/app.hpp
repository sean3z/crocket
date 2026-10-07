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

/// Framework behaviour that is not about the socket.
struct Config {
  std::size_t max_body_bytes = 1 << 20;               // 413 body.too_large
  std::size_t max_in_flight = 1024;                   // 503 server.busy
  std::chrono::milliseconds request_timeout{30'000};  // 504 deadline.exceeded
  std::chrono::milliseconds drain_timeout{10'000};    // graceful shutdown budget
  bool debug_routes = false;                          // expose GET /__routes
  Profile profile = Profile::Release;

  /// Proxies in front of the server, as addresses or CIDR ranges ("10.0.0.0/8",
  /// "fd00::/8"). Requests from them have Request::remote_addr and scheme taken
  /// from `proxy_header`; that header is ignored from everyone else.
  std::vector<std::string> trusted_proxies = {};
  ProxyHeader proxy_header = ProxyHeader::XForwardedFor;
  /// Host names this server answers to ("api.example.com", "*.example.com").
  /// Any other Host is 400 host.invalid. Empty allows every host; the dev
  /// profile also allows localhost.
  std::vector<std::string> allowed_hosts = {};
  json::ReadOptions json = {};                        // limits for Json<T> bodies
  /// Run a plain-function route on the event loops once it has proved fast
  /// (1,000 runs in a row under 100 us), and back on workers the first time it
  /// takes over 1 ms there. Off: plain functions always run on workers.
  bool adaptive_placement = true;
  log::Options log = {};                              // level, sampling, redaction, sink

  /// Dev defaults: GET /__routes, a one-hour deadline (room for a debugger
  /// breakpoint), a one-second drain, and listening on 127.0.0.1.
  static Config dev();
  /// dev() when CROCKET_PROFILE=dev; the defaults when it is unset or "release".
  /// Throws std::invalid_argument for any other value.
  static Config from_env();
};

/// Socket, TLS and threading. TLS is enabled when both cert and key are set.
struct LaunchOptions {
  std::string host = {};  // empty: 0.0.0.0, or 127.0.0.1 in the dev profile
  std::uint16_t port = 8000;
  std::string tls_cert = {};
  std::string tls_key = {};
  unsigned workers = 0;                  // handler threads; 0 = hardware concurrency
  unsigned event_loops = 0;              // network I/O threads; 0 = hardware concurrency
  std::size_t max_header_bytes = 8192;   // names + values; 431 headers.too_large
  std::size_t max_header_count = 100;    // 431 headers.too_large
  bool http2 = true;                     // offer h2 via ALPN when TLS is on
  /// Without TLS, also accept HTTP/2 with prior knowledge (and Upgrade: h2c), as
  /// gRPC clients use on an insecure channel. HTTP/1.1 keeps working on the port.
  bool h2_prior_knowledge = false;
};

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
  static constexpr std::chrono::milliseconds kDemoteAfter{1};        // one run on a loop over this
  static constexpr std::chrono::seconds kRetryAfter{10};             // doubled per demotion
  std::atomic<bool> on_loop{false};
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

/// Counters the framework maintains itself (read by Metrics).
struct Stats {
  std::atomic<std::int64_t> in_flight{0};
  std::atomic<bool> draining{false};
  std::atomic<std::uint64_t> log_dropped{0};
};

/// An address range from Config::trusted_proxies (IPv4 as IPv4-mapped IPv6).
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
    auto name = detail::type_display_name<T>();
    if (!core_->state.put<T>(std::move(value)))
      core_->build_errors.push_back(std::string("manage<") + name + ">() called twice");
    else if constexpr (requires(const T& t) { { t.ready() } -> std::convertible_to<bool>; })
      core_->ready_checks.push_back({name, detail::type_key<T>(),
                                     [](void* p) -> bool { return static_cast<const T*>(p)->ready(); }});
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
  int launch(LaunchOptions opts);

  /// The full request pipeline: request id, fairings, routing, extractors,
  /// handler, responder, error mapping. `done` gets the response exactly once:
  /// before handle_async returns, or later, when an async handler finishes.
  /// Called on an event loop (`loop`), async handlers run and resume there, and
  /// a synchronous handler moves to a worker. Used by the engine.
  void handle_async(Request&& req, std::move_only_function<void(Response&&)> done, detail::Executor* loop = nullptr);
  /// Whether every route that can answer method+path runs on the event loop
  /// (an async handler, or a plain function adaptive placement has promoted),
  /// so the loop can run the request itself.
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
  /// Routes req; true if the handler suspended (it completes the exchange later).
  bool run_routes(Request& req, Response& res);
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
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view q);
std::string generate_request_id();
std::string_view reason_phrase(int status);
std::string iso8601_now();
}  // namespace detail

}  // namespace crocket
