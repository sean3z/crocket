#pragma once
// Outbound HTTP. Opt-in: link crocket::client. crocket.hpp does not include this.
//
//   using namespace std::chrono_literals;
//
//   auto billing = http::Client{{
//       .base = "https://billing.internal",
//       .timeout = 2s,
//       .pool = 64,
//       .retry = retry::idempotent(3),
//       .breaker = breaker::consecutive(5, 30s),
//   }};
//
//   auto r = co_await billing.get("/invoices/" + id);
//   auto once = co_await billing.post("/charges", {.body = "{}", .retry = retry::none});
//
// ClientOptions and CallOptions are aggregates on purpose. Designated initializers
// are how callers set them, including the per-call opt-outs above. They stay
// aggregates: no constructors, no bases, no std::optional members.
//
// Everything here returns Task. There is no blocking call. Retries and the
// circuit breaker are on unless a call turns them off.

#include "crocket/async.hpp"
#include "crocket/json.hpp"
#include "crocket/task.hpp"

#include <map>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crocket {

namespace retry {

/// How a call is repeated. `attempts` is the number of tries after the first,
/// so `idempotent(3)` is four tries in all and `none` is one. `attempts < 0`
/// means "use the client's policy" (`inherit`).
///
/// `budget` is the share of recent calls to this upstream that may be retries
/// (0.2 is 20%). `budget_min` retries are allowed in the window even when
/// traffic is too low for the percentage to permit one, so a quiet upstream
/// can still retry.
struct Policy {
  int attempts = 3;
  bool idempotent_only = true;
  double budget = 0.2;
  int budget_min = 3;
  std::chrono::milliseconds initial{20};
  std::chrono::milliseconds max_delay{1'000};
};

inline constexpr Policy none{.attempts = 0};
inline constexpr Policy inherit{.attempts = -1};

constexpr Policy idempotent(int attempts = 3) {
  Policy p;
  p.attempts = attempts;
  p.idempotent_only = true;
  return p;
}

}  // namespace retry

namespace breaker {

enum class Mode : std::uint8_t { Off, Inherit, Consecutive, Rate };

/// When to stop calling an upstream that is failing. One breaker per client.
/// `consecutive` opens after that many failures in a row. `rate` opens when
/// that fraction of the last `window` calls failed, once `min_samples` of them
/// have been seen. `open_for` is how long it stays open the first time; each
/// failed half-open probe multiplies that by `backoff`, up to `max_open`.
struct Policy {
  Mode mode = Mode::Consecutive;
  int consecutive = 5;
  double failure_rate = 0.5;
  int window = 20;
  int min_samples = 10;
  std::chrono::milliseconds open_for{30'000};
  int half_open_probes = 1;
  int backoff = 2;
  std::chrono::milliseconds max_open{300'000};
};

inline constexpr Policy none{.mode = Mode::Off};
inline constexpr Policy inherit{.mode = Mode::Inherit};

constexpr Policy consecutive(int failures, std::chrono::milliseconds open_for) {
  Policy p;
  p.mode = Mode::Consecutive;
  p.consecutive = failures;
  p.open_for = open_for;
  return p;
}

constexpr Policy rate(double failure_rate, int window, std::chrono::milliseconds open_for) {
  Policy p;
  p.mode = Mode::Rate;
  p.failure_rate = failure_rate;
  p.window = window < 1 ? 1 : window;
  p.min_samples = p.window < 2 ? 1 : p.window / 2;
  p.open_for = open_for;
  return p;
}

}  // namespace breaker

namespace http {

/// What `manage(http::Client{{...}})` takes. `name` is the Prometheus label
/// (`upstream`); empty uses the host from `base`, never the URL. `pool` is how
/// many connections to that host may be open. `timeout` bounds one call.
/// `connect_timeout` bounds the TCP connect. A call can only shorten `timeout`.
/// Logging this object prints scheme, host, and port, and whether `ca_file`
/// is set. It does not print userinfo, path, query, fragment, or the CA path.
struct ClientOptions {
  std::string base{};
  std::string name{};
  std::chrono::milliseconds timeout{2'000};
  std::chrono::milliseconds connect_timeout{500};
  std::size_t pool = 64;
  retry::Policy retry = retry::idempotent(3);
  breaker::Policy breaker = breaker::consecutive(5, std::chrono::milliseconds{30'000});
  /// Copy the inbound `x-request-id` onto the upstream request when the caller
  /// sent one. A generated id is not invented here, and `traceparent` is not sent.
  bool propagate_request_id = true;
  bool verify_tls = true;
  std::string ca_file{};
  /// Largest response body kept. A bigger one fails the call.
  std::size_t max_body = std::size_t{1} << 20;
};

/// Per-call overrides. Omitted fields keep the client's policy. `timeout` of
/// `milliseconds::max()` (the default) means the client's timeout. `retry::none`
/// and `breaker::none` turn those off for this call only. Any other breaker
/// value still uses the client's breaker; the thresholds live on the client.
struct CallOptions {
  std::string body{};
  std::vector<std::pair<std::string, std::string>> headers{};
  retry::Policy retry = retry::inherit;
  breaker::Policy breaker = breaker::inherit;
  std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
  /// Sent as `Idempotency-Key` when set. A POST or PATCH with this key may be retried.
  std::string idempotency_key{};
};

/// Scheme, host, and port. Userinfo, path, query, and fragment are left out.
/// Anything that is not an absolute http(s) URL becomes "[invalid]", so a
/// secret in a bad string is not printed back.
inline std::string log_endpoint(std::string_view url) {
  auto scheme = url.find("://");
  if (scheme == std::string_view::npos || scheme == 0) return "[invalid]";
  auto rest = url.substr(scheme + 3);
  auto cut = rest.find_first_of("/?#");
  auto authority = cut == std::string_view::npos ? rest : rest.substr(0, cut);
  if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
  if (authority.empty()) return "[invalid]";
  std::string out;
  out.reserve(scheme + 3 + authority.size());
  out.append(url.substr(0, scheme + 3));
  out.append(authority);
  return out;
}

/// Path with query and fragment removed. An absolute URL keeps only
/// scheme, host, and port.
inline std::string log_target(std::string_view target) {
  if (target.find("://") != std::string_view::npos) return log_endpoint(target);
  auto cut = target.find_first_of("?#");
  return std::string(cut == std::string_view::npos ? target : target.substr(0, cut));
}

/// What `log::info("{opts}", options)` prints. The CA path is only a bool.
struct OptionsLog {
  std::string base;
  std::string name;
  std::chrono::milliseconds timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::size_t pool = 0;
  retry::Policy retry{};
  breaker::Policy breaker{};
  bool propagate_request_id = false;
  bool verify_tls = false;
  bool ca_file = false;
  std::size_t max_body = 0;
};

inline OptionsLog for_log(const ClientOptions& options) {
  OptionsLog out;
  out.base = log_endpoint(options.base);
  out.name = options.name;
  out.timeout = options.timeout;
  out.connect_timeout = options.connect_timeout;
  out.pool = options.pool;
  out.retry = options.retry;
  out.breaker = options.breaker;
  out.propagate_request_id = options.propagate_request_id;
  out.verify_tls = options.verify_tls;
  out.ca_file = !options.ca_file.empty();
  out.max_body = options.max_body;
  return out;
}

/// Diagnostic form of `ClientOptions`. Logging the options object uses this.
inline void crocket_json_write(std::string& out, const ClientOptions& options) {
  json::encode(out, for_log(options));
}

/// A call as it is safe to log. The body is a byte count. The idempotency
/// key is only whether one was set.
struct CallLog {
  std::string target;
  std::map<std::string, std::string> headers;
  std::size_t body_bytes = 0;
  bool idempotency_key = false;
};

inline CallLog for_log(std::string_view target, const CallOptions& call) {
  CallLog out;
  out.target = log_target(target);
  Headers headers;
  for (auto& [name, value] : call.headers) headers.add(name, value);
  if (!call.idempotency_key.empty()) headers.add("idempotency-key", call.idempotency_key);
  out.headers = for_log(headers);
  out.body_bytes = call.body.size();
  out.idempotency_key = !call.idempotency_key.empty();
  return out;
}

/// A response as it is safe to log. The body is a byte count.
struct ResponseLog {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::size_t body_bytes = 0;
};

inline ResponseLog for_log(const Response& response) {
  return ResponseLog{response.status, for_log(response.headers), response.body.size()};
}

namespace detail {
struct ClientState;
/// Owns the client state. The destructor is out of line so callers can hold a
/// client without seeing the state type.
struct ClientHandle {
  std::shared_ptr<ClientState> p;
  ClientHandle() noexcept = default;
  explicit ClientHandle(std::shared_ptr<ClientState> s) noexcept : p(std::move(s)) {}
  ClientHandle(ClientHandle&&) noexcept = default;
  ClientHandle& operator=(ClientHandle&&) noexcept = default;
  ClientHandle(const ClientHandle&) = delete;
  ClientHandle& operator=(const ClientHandle&) = delete;
  ~ClientHandle();
};
ClientHandle make_client(ClientOptions options);
void shutdown_client(ClientHandle& handle);
Task<std::expected<Response, ApiError>> client_request(const ClientHandle& handle, Method method, std::string_view path,
                                                       CallOptions options);

/// Shared by `Client` and `Client::For<Tag>`. Not a state type.
class ClientBase {
 public:
  ClientBase(const ClientBase&) = delete;
  ClientBase& operator=(const ClientBase&) = delete;
  ClientBase& operator=(ClientBase&&) = delete;

  [[nodiscard]] Task<std::expected<Response, ApiError>> request(Method method, std::string_view path,
                                                                CallOptions options = {}) const {
    return client_request(impl_, method, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> get(std::string_view path, CallOptions options = {}) const {
    return request(Method::Get, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> head(std::string_view path, CallOptions options = {}) const {
    return request(Method::Head, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> post(std::string_view path, CallOptions options = {}) const {
    return request(Method::Post, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> put(std::string_view path, CallOptions options = {}) const {
    return request(Method::Put, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> patch(std::string_view path, CallOptions options = {}) const {
    return request(Method::Patch, path, std::move(options));
  }
  [[nodiscard]] Task<std::expected<Response, ApiError>> del(std::string_view path, CallOptions options = {}) const {
    return request(Method::Delete, path, std::move(options));
  }

 protected:
  explicit ClientBase(ClientOptions options) : impl_(make_client(std::move(options))) {}
  explicit ClientBase(ClientHandle handle) : impl_(std::move(handle)) {}
  ClientBase(ClientBase&&) noexcept = default;
  ~ClientBase() { shutdown_client(impl_); }

  ClientHandle impl_;
};

}  // namespace detail

/// One managed client. `State<http::Client>` is one upstream: a second
/// `.manage(http::Client)` fails at ignite. Another service is
/// `State<http::Client::For<Payments>>`, from `.with_state<Payments>()`.
class Client : public detail::ClientBase {
 public:
  using Options = ClientOptions;

  /// Throws std::invalid_argument when `base` is not an absolute http or https URL.
  explicit Client(ClientOptions options) : detail::ClientBase(std::move(options)) {}
  Client(Client&&) noexcept = default;

  template <class Tag>
  class For;

  /// Moves this client into `State<http::Client::For<Tag>>`.
  template <class Tag>
  [[nodiscard]] For<Tag> with_state() &&;
};

template <class Tag>
class Client::For : public detail::ClientBase {
  friend class Client;
  explicit For(detail::ClientHandle handle) : ClientBase(std::move(handle)) {}

 public:
  For(For&&) noexcept = default;
};

template <class Tag>
Client::For<Tag> Client::with_state() && {
  return For<Tag>(std::move(impl_));
}

/// Decodes a response body the same way `Json<T>` reads a request. The HTTP
/// status is not checked: a 404 body is still parsed when it is JSON.
template <class T>
std::expected<T, ApiError> json(const Response& response) {
  auto parsed = crocket::json::from_string<T>(response.body);
  if (!parsed) {
    ApiError err = ApiError::unprocessable("json.invalid", parsed.error().describe());
    err.errors = std::move(parsed.error().errors);
    return std::unexpected(std::move(err));
  }
  return std::move(*parsed);
}

}  // namespace http
}  // namespace crocket
