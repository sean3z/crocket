#pragma once
// Responder<T>: how a handler's return value becomes a Response.
// Specialize `crocket::Responder<MyType>` with
//   static void respond(MyType&&, const Request&, Response&);
// to make a new return type legal.

#include "crocket/json.hpp"
#include "crocket/request.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace crocket {

template <class T>
struct Responder;  // undefined: an unknown return type does not compile

template <class T>
concept IsResponder = requires(std::remove_cvref_t<T>&& v, const Request& rq, Response& rs) {
  Responder<std::remove_cvref_t<T>>::respond(std::move(v), rq, rs);
};

template <class T, class E = ApiError>
using Result = std::expected<T, E>;

/// Writes the standard error body and records code/detail for logs+metrics.
void write_error(const ApiError& e, const Request& rq, Response& rs);

namespace detail {
template <class T>
void respond_value(T&& v, const Request& rq, Response& rs) {
  using U = std::remove_cvref_t<T>;
  if constexpr (IsResponder<U>) Responder<U>::respond(std::forward<T>(v), rq, rs);
  else if constexpr (json::Encodable<U>) Responder<Json<U>>::respond(Json<U>{std::forward<T>(v)}, rq, rs);
  else static_assert(false, "crocket: value is neither a Responder nor JSON-encodable");
}
template <class T>
concept Payload = IsResponder<T> || json::Encodable<T>;
}  // namespace detail

// ---- status wrappers --------------------------------------------------------
// The payload may be any Responder or any JSON-encodable value (sent as JSON).

template <int Code, class T>
struct WithStatus {
  T value;
  std::string location = {};  // optional Location header
};

template <class T> using Created = WithStatus<201, T>;
template <class T> using Accepted = WithStatus<202, T>;

struct NoContent {};

/// What a client may use to revalidate or resume a response (see Cacheable).
struct CacheOptions {
  /// Sent as Last-Modified; If-Modified-Since then answers 304 when nothing changed.
  std::optional<std::chrono::system_clock::time_point> last_modified = {};
  /// Serve byte ranges: Range answers 206 with part of the body.
  bool ranges = false;
};

/// A payload plus caching options: `return Cacheable{body, {.last_modified = t, .ranges = true}};`.
/// Every successful GET gets an ETag and 304s without this.
template <class T>
struct Cacheable {
  T value;
  CacheOptions cache = {};
};
template <class T>
Cacheable(T, CacheOptions = {}) -> Cacheable<T>;

/// Runtime status + payload, for when the status is data-dependent.
template <class T>
struct Status {
  int code;
  T value;
};

// ---- specializations --------------------------------------------------------

template <>
struct Responder<std::string> {
  static void respond(std::string&& s, const Request&, Response& rs) {
    rs.set_content_type("text/plain; charset=utf-8");
    rs.body = std::move(s);
  }
};
template <>
struct Responder<std::string_view> {
  static void respond(std::string_view&& s, const Request&, Response& rs) {
    rs.set_content_type("text/plain; charset=utf-8");
    rs.body.assign(s);
  }
};
template <>
struct Responder<const char*> {
  static void respond(const char*&& s, const Request&, Response& rs) {
    rs.set_content_type("text/plain; charset=utf-8");
    rs.body.assign(s);
  }
};

template <json::Encodable T>
struct Responder<Json<T>> {
  static void respond(Json<T>&& j, const Request&, Response& rs) {
    rs.set_content_type("application/json");
    rs.body.clear();
    json::encode(rs.body, j.value);
  }
};

template <>
struct Responder<ApiError> {
  static void respond(ApiError&& e, const Request& rq, Response& rs) { write_error(e, rq, rs); }
};

template <>
struct Responder<NoContent> {
  static void respond(NoContent&&, const Request&, Response& rs) {
    rs.status = 204;
    rs.body.clear();
  }
};

/// Raw escape hatch: return a fully built Response.
template <>
struct Responder<Response> {
  static void respond(Response&& r, const Request&, Response& rs) { rs = std::move(r); }
};

template <int Code, detail::Payload T>
struct Responder<WithStatus<Code, T>> {
  static void respond(WithStatus<Code, T>&& w, const Request& rq, Response& rs) {
    detail::respond_value(std::move(w.value), rq, rs);
    if (rs.error_code.empty()) {
      rs.status = Code;
      if (!w.location.empty()) rs.headers.set("location", w.location);
    }
  }
};

template <detail::Payload T>
struct Responder<Cacheable<T>> {
  static void respond(Cacheable<T>&& c, const Request& rq, Response& rs) {
    detail::respond_value(std::move(c.value), rq, rs);
    if (!rs.error_code.empty()) return;
    if (c.cache.last_modified) rs.headers.set("last-modified", http::date(*c.cache.last_modified));
    if (c.cache.ranges) rs.headers.set("accept-ranges", "bytes");
  }
};

template <detail::Payload T>
struct Responder<Status<T>> {
  static void respond(Status<T>&& s, const Request& rq, Response& rs) {
    detail::respond_value(std::move(s.value), rq, rs);
    if (rs.error_code.empty()) rs.status = s.code;
  }
};

/// Empty optional → 404 not_found.
template <detail::Payload T>
struct Responder<std::optional<T>> {
  static void respond(std::optional<T>&& o, const Request& rq, Response& rs) {
    if (o) detail::respond_value(std::move(*o), rq, rs);
    else write_error(ApiError::not_found(), rq, rs);
  }
};

template <class T, detail::Payload E>
  requires(std::is_void_v<T> || detail::Payload<T>)
struct Responder<std::expected<T, E>> {
  static void respond(std::expected<T, E>&& r, const Request& rq, Response& rs) {
    if (r) {
      if constexpr (std::is_void_v<T>) Responder<NoContent>::respond(NoContent{}, rq, rs);
      else detail::respond_value(std::move(*r), rq, rs);
    } else {
      detail::respond_value(std::move(r.error()), rq, rs);
    }
  }
};

}  // namespace crocket
