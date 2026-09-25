#pragma once
// The application: managed state, fairings, mounted routes, ignite, listen.

#include "crocket/detail/invoke.hpp"
#include "crocket/http.hpp"
#include "crocket/request.hpp"
#include "crocket/state.hpp"

#include <atomic>
#include <chrono>
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

/// Framework behaviour that is not about the socket.
struct Config {
  std::size_t max_body_bytes = 1 << 20;               // 413 body.too_large
  std::size_t max_in_flight = 1024;                   // 503 server.busy
  std::chrono::milliseconds request_timeout{30'000};  // 504 deadline.exceeded
  std::chrono::milliseconds drain_timeout{10'000};    // graceful shutdown budget
  bool debug_routes = false;                          // expose GET /__routes
  Profile profile = Profile::Release;

  /// Dev defaults: GET /__routes, a one-hour deadline (room for a debugger
  /// breakpoint), a one-second drain, and listening on 127.0.0.1.
  static Config dev();
  /// dev() when CROCKET_PROFILE=dev; the defaults when it is unset or "release".
  /// Throws std::invalid_argument for any other value.
  static Config from_env();
};

/// Socket, TLS and threading. TLS is enabled when both cert and key are set.
struct ListenOptions {
  std::string host = {};  // empty: 0.0.0.0, or 127.0.0.1 in the dev profile
  std::uint16_t port = 8000;
  std::string tls_cert = {};
  std::string tls_key = {};
  unsigned workers = 0;                  // 0 = hardware concurrency
  std::size_t max_header_bytes = 8192;   // lws header buffer (max 65535)
  bool http2 = true;                     // offer h2 via ALPN when TLS is on
};

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
};

using Routes = std::vector<RouteDef>;

struct IgniteError {
  std::vector<std::string> problems;
  [[nodiscard]] std::string message() const;
};

class App;
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

/// Counters the framework maintains itself (read by Metrics).
struct Stats {
  std::atomic<std::int64_t> in_flight{0};
  std::atomic<bool> draining{false};
};

struct AppCore {
  Config config;
  StateRegistry state;
  std::vector<std::unique_ptr<FairingBase>> fairings;
  std::vector<ReadyCheck> ready_checks;
  std::vector<std::pair<std::string, Routes>> mounts;
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

 private:
  friend class App;
  Ignite(detail::AppCore& c, std::vector<std::string>& e) : core_(c), errors_(e) {}
  detail::AppCore& core_;
  std::vector<std::string>& errors_;
};

class App {
 public:
  explicit App(Config cfg = {});
  App(App&&) noexcept;
  App& operator=(App&&) noexcept;
  ~App();

  /// Store exactly one T. A second manage<T>() is an ignite error.
  /// If T has `bool ready() const`, GET /readyz calls it.
  template <class T>
  App& manage(T value) & {
    auto name = detail::type_display_name<T>();
    if (!core_->state.put<T>(std::move(value)))
      core_->build_errors.push_back(std::string("manage<") + name + ">() called twice");
    else if constexpr (requires(const T& t) { { t.ready() } -> std::convertible_to<bool>; })
      core_->ready_checks.push_back({name, detail::type_key<T>(),
                                     [](void* p) -> bool { return static_cast<const T*>(p)->ready(); }});
    return *this;
  }
  template <class T>
  App&& manage(T value) && { return std::move(manage(std::move(value))); }

  /// Attach a fairing. on_request runs in attach order, on_response in
  /// reverse. Fairings run on worker threads concurrently: keep them short,
  /// synchronous and thread-safe.
  template <class F>
  App& attach(F fairing) & {
    core_->fairings.push_back(std::make_unique<detail::FairingModel<F>>(std::move(fairing)));
    return *this;
  }
  template <class F>
  App&& attach(F fairing) && { return std::move(attach(std::move(fairing))); }

  App& mount(std::string_view base, Routes routes) &;
  App&& mount(std::string_view base, Routes routes) && { return std::move(mount(base, std::move(routes))); }

  App& configure(Config cfg) &;
  App&& configure(Config cfg) && { return std::move(configure(std::move(cfg))); }

  /// Validate everything and freeze the route table. Idempotent.
  std::expected<void, IgniteError> ignite();

  /// Ignite, serve until SIGINT/SIGTERM, drain, shut down. Returns an exit
  /// status (non-zero when ignite or listen fails).
  int listen(ListenOptions opts);

  /// The full request pipeline: request id, fairings, routing, extractors,
  /// handler, responder, error mapping. Used by the engine and LocalClient.
  Response handle(Request req);

  /// Build an error response for a request rejected before routing
  /// (limits, timeouts). Runs on_response fairings so logs/metrics see it.
  Response reject(Request& req, const ApiError& err);

  /// Assign request id, deadline and state pointer. Idempotent.
  void prepare(Request& req) const;

  [[nodiscard]] const Config& config() const { return core_->config; }
  [[nodiscard]] std::span<const RouteDef> routes() const { return core_->routes; }
  [[nodiscard]] detail::AppCore& core() { return *core_; }

 private:
  Response run_routes(Request& req);
  Response builtin(detail::Builtin b, Request& req);
  void finish(const Request& req, Response& res);
  std::unique_ptr<detail::AppCore> core_;
};

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
    Call& bearer(std::string_view token) { return header("authorization", std::string("Bearer ") + std::string(token)); }
    Response dispatch() { return app_.handle(std::move(req_)); }

   private:
    friend class LocalClient;
    Call(App& app, http::Method m, std::string_view target);
    App& app_;
    Request req_;
  };

  explicit LocalClient(App& app);
  Call get(std::string_view target) { return {app_, http::Method::Get, target}; }
  Call head(std::string_view target) { return {app_, http::Method::Head, target}; }
  Call post(std::string_view target) { return {app_, http::Method::Post, target}; }
  Call put(std::string_view target) { return {app_, http::Method::Put, target}; }
  Call patch(std::string_view target) { return {app_, http::Method::Patch, target}; }
  Call del(std::string_view target) { return {app_, http::Method::Delete, target}; }
  Call options(std::string_view target) { return {app_, http::Method::Options, target}; }

 private:
  App& app_;
};

namespace detail {
std::string percent_decode(std::string_view s, bool plus_is_space);
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view q);
std::string generate_request_id();
std::string_view reason_phrase(int status);
std::string iso8601_now();
}  // namespace detail

}  // namespace crocket
