#pragma once
// Request / Response as the framework sees them. The engine fills a Request,
// Crocket turns it into a Response. Handlers normally never touch either:
// they take extractors and return responders. `const Request&` and
// `Response&` parameters exist as an escape hatch.

#include "crocket/http.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace crocket {

/// One invalid field of a request, listed under "errors" in the problem body.
struct FieldError {
  std::string pointer;  // RFC 6901 JSON Pointer into the request body: "/items/0/name"
  std::string detail;   // "must be at least 1"
};

/// The error shape for every failure the framework or a handler reports, sent
/// as RFC 9457 application/problem+json. `message` goes to the client (as
/// "detail"); `detail` only goes to logs, and to the body in the dev profile.
struct ApiError {
  int status = 500;
  std::string_view code = "internal";  // stable, dotted: "auth.expired"
  std::string message = "internal error";
  std::string detail = {};
  int grpc_status = -1;  // explicit gRPC status code; -1 derives it from `status`
  std::vector<FieldError> errors = {};  // per-field failures, e.g. from validation
  std::string_view type = {};           // problem type URI; empty means "about:blank"

  static ApiError not_found(std::string msg = "not found") { return {404, "not_found", std::move(msg), {}}; }
  static ApiError bad_request(std::string_view code, std::string msg) { return {400, code, std::move(msg), {}}; }
  static ApiError unprocessable(std::string_view code, std::string msg) { return {422, code, std::move(msg), {}}; }
  static ApiError unauthorized(std::string_view code, std::string msg) { return {401, code, std::move(msg), {}}; }
  static ApiError forbidden(std::string_view code, std::string msg) { return {403, code, std::move(msg), {}}; }
  static ApiError unavailable(std::string_view code, std::string msg) { return {503, code, std::move(msg), {}}; }
  static ApiError internal(std::string detail) { return {500, "internal", "internal error", std::move(detail)}; }
};

namespace json {
/// Limits and strictness for reading JSON. Config::json sets them for every
/// Json<T> body; exceeding a limit is 422 json.limit_exceeded.
struct ReadOptions {
  std::size_t max_depth = 64;                  // nested arrays and objects
  std::size_t max_string_bytes = 1 << 20;      // one string or key, after unescaping
  std::size_t max_object_members = 1024;       // members of one object
  std::size_t max_array_elements = 100'000;    // elements of one array
  bool deny_unknown_fields = false;            // as if every struct had [[= json::deny_unknown_fields]]
};
}  // namespace json

using Clock = std::chrono::steady_clock;

/// Deadline and cancellation for one request. Available to blocking
/// extractors (e.g. pool checkout) and to handlers as an extractor.
/// The stop token fires when the client disconnects, the deadline passes
/// (engine timer), or the server stops draining.
struct Deadline {
  Clock::time_point at = Clock::time_point::max();
  std::stop_token stop = {};

  [[nodiscard]] bool expired() const { return stop.stop_requested() || Clock::now() >= at; }
  [[nodiscard]] Clock::duration remaining() const {
    auto now = Clock::now();
    return at > now ? at - now : Clock::duration::zero();
  }
};

namespace detail { class StateRegistry; }

struct Request {
  http::Method method = http::Method::Get;
  std::string method_text = "GET";  // as received (for unknown methods)
  std::string path = "/";           // percent-decoded, no query string
  std::vector<std::pair<std::string, std::string>> query;  // decoded key/value pairs
  Headers headers;
  std::string body;

  // Connection facts, set by the engine.
  std::string_view protocol = "local";  // "http/1.1", "h2", or "local"
  bool tls = false;
  std::string peer_addr;  // the socket's peer: the client, or the nearest proxy

  // The client, as Config::trusted_proxies report it; otherwise the connection's.
  std::string remote_addr;  // e.g. "203.0.113.9"
  std::string_view scheme;  // "https" or "http"

  // Set by the framework before fairings run.
  std::string request_id;
  Clock::time_point received = Clock::now();
  Deadline deadline;
  bool dev_profile = false;  // error bodies include ApiError::detail
  json::ReadOptions json_options;  // Config::json

  // Set by the router for the candidate currently being tried.
  std::string_view route_template;  // "/users/{id}"; "" while unmatched
  std::string_view handler;         // "api::create"
  std::vector<std::string_view> captures;

  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
    return headers.get(name);
  }
  /// The raw (decoded) text of the capture `{name}` in the matched route, if the
  /// route has one. Handlers normally take captures as parameters instead.
  [[nodiscard]] std::optional<std::string_view> capture(std::string_view name) const {
    std::size_t index = 0;
    std::string_view rest = route_template;
    while (!rest.empty()) {
      auto slash = rest.find('/');
      auto seg = rest.substr(0, slash);
      if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
        if (seg.substr(1, seg.size() - 2) == name)
          return index < captures.size() ? std::optional<std::string_view>(captures[index]) : std::nullopt;
        ++index;
      }
      if (slash == std::string_view::npos) break;
      rest.remove_prefix(slash + 1);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string_view> query_param(std::string_view key) const {
    for (auto& [k, v] : query)
      if (k == key) return std::string_view(v);
    return std::nullopt;
  }

  // Framework-internal: managed state lookup for State<T>.
  const detail::StateRegistry* state_registry = nullptr;
};

struct Response {
  // For a gRPC response (content-type application/grpc) the wire status is
  // always 200 and the outcome travels in grpc-status; `status` then holds the
  // HTTP equivalent of that outcome, so logs and metrics read the same either way.
  int status = 200;
  Headers headers;
  std::string body;
  Headers trailers;  // sent after the body; HTTP/2 only (gRPC needs them)

  // Observability, never sent to the client.
  std::string_view error_code;    // set for error responses
  std::string error_detail;       // ApiError::detail, logs only
  std::string_view failure_kind;  // extractor failure kind: "path", "json", "auth", ...

  void set_content_type(std::string_view ct) { headers.set("content-type", ct); }
};

}  // namespace crocket
