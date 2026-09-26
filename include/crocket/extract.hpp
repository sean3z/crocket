#pragma once
// Two kinds of handler parameters:
//
//   * Path parameters: `{name}` in the route binds to the parameter `name`.
//     Its type must specialize FromParam<T>. A parse failure forwards to the
//     next ranked route, and ends in 404 path.invalid if none accepts.
//
//   * Extractors: every other parameter. Its type must specialize
//     FromRequest<T>:
//       static constexpr std::string_view kind;        // "json", "auth", ...
//       static constexpr bool consumes_body;            // at most one per route
//       static std::expected<T, ApiError> extract(Request&);
//       static void state_deps(std::vector<detail::StateDep>&);   // optional
//     Declared state dependencies are verified at ignite.

#include "crocket/json.hpp"
#include "crocket/request.hpp"
#include "crocket/state.hpp"

#include <charconv>
#include <concepts>
#include <expected>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace crocket {

// ---- path parameters --------------------------------------------------------

template <class T>
struct FromParam;  // undefined: type cannot bind to a path capture

template <>
struct FromParam<std::string_view> {
  // Views into Request::path, which outlives the handler call.
  static std::optional<std::string_view> parse(std::string_view s) { return s; }
};
template <>
struct FromParam<std::string> {
  static std::optional<std::string> parse(std::string_view s) { return std::string(s); }
};
template <>
struct FromParam<bool> {
  static std::optional<bool> parse(std::string_view s) {
    if (s == "true") return true;
    if (s == "false") return false;
    return std::nullopt;
  }
};
template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct FromParam<T> {
  // Parsed as a number even for 8-bit types: "/age/7" means 7, not '7'.
  static std::optional<T> parse(std::string_view s) {
    using Wide = std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>;
    if (s.empty() || s.front() == '+') return std::nullopt;
    Wide w{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), w);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    if (w < static_cast<Wide>(std::numeric_limits<T>::min()) ||
        w > static_cast<Wide>(std::numeric_limits<T>::max()))
      return std::nullopt;
    return static_cast<T>(w);
  }
};
template <class T>
  requires std::is_floating_point_v<T>
struct FromParam<T> {
  static std::optional<T> parse(std::string_view s) {
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
  }
};

template <class T>
concept PathParam = requires(std::string_view s) {
  { FromParam<T>::parse(s) } -> std::same_as<std::optional<T>>;
};

// ---- extractors -------------------------------------------------------------

template <class T>
struct FromRequest;  // undefined: unknown parameter types do not compile

template <class T>
concept Extractor = requires(Request& r) {
  { FromRequest<T>::extract(r) } -> std::same_as<std::expected<T, ApiError>>;
  { FromRequest<T>::kind } -> std::convertible_to<std::string_view>;
  { FromRequest<T>::consumes_body } -> std::convertible_to<bool>;
};

namespace detail {
template <class T>
void collect_state_deps(std::vector<StateDep>& out) {
  if constexpr (requires { FromRequest<T>::state_deps(out); }) FromRequest<T>::state_deps(out);
}
inline bool is_json_media_type(std::string_view ct) {
  auto semi = ct.find(';');
  auto mt = ct.substr(0, semi);
  while (!mt.empty() && mt.back() == ' ') mt.remove_suffix(1);
  if (Headers::iequals(mt, "application/json")) return true;
  return mt.size() > 5 && Headers::iequals(mt.substr(mt.size() - 5), "+json");
}
}  // namespace detail

/// State<T>: managed singleton. Missing .manage(T) fails at ignite.
template <class T>
struct FromRequest<State<T>> {
  static constexpr std::string_view kind = "state";
  static constexpr bool consumes_body = false;
  static std::expected<State<T>, ApiError> extract(Request& r) {
    // Ignite guarantees presence; the null check guards misuse outside Crocket.
    T* p = r.state_registry ? r.state_registry->template get<T>() : nullptr;
    if (!p) return std::unexpected(ApiError::internal("State<T> requested but not managed"));
    return State<T>(*p);
  }
  static void state_deps(std::vector<detail::StateDep>& out) { out.push_back(detail::state_dep<T>()); }
};

/// Json<T>: parses the body. 415 on a non-JSON content type, 422 json.invalid
/// on missing/malformed JSON or a shape mismatch.
namespace detail {

consteval bool has_from_path(std::meta::info member) {
  return !std::meta::annotations_of_with_type(member, ^^json::FromPath).empty();
}

/// Applies [[= json::from_path]] members of the top-level body struct T.
template <class T>
std::expected<void, ApiError> apply_from_path(const Request& rq, T& value) {
  if constexpr (std::is_class_v<T> && std::is_aggregate_v<T>) {
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx))) {
      if constexpr (has_from_path(m)) {
        using M = [:std::meta::remove_cvref(std::meta::type_of(m)):];
        constexpr std::string_view name = std::meta::identifier_of(m);
        static_assert(json::is_optional<M>::value,
                      std::string("crocket: [[= json::from_path]] member '") + std::string(name) + "' of " +
                          std::string(std::meta::display_string_of(^^T)) +
                          " must be std::optional<P>: it is absent on routes without a {" + std::string(name) +
                          "} capture");
        using P = typename M::value_type;
        static_assert(PathParam<P>, std::string("crocket: [[= json::from_path]] member '") + std::string(name) +
                                        "' has type " + std::string(std::meta::display_string_of(^^P)) +
                                        ", which cannot be parsed from a path segment (no FromParam)");
        static_assert(std::equality_comparable<P>,
                      std::string("crocket: [[= json::from_path]] member '") + std::string(name) +
                          "' must be equality-comparable to check it against the URL");

        auto& field = value.[:m:];
        if (auto raw = rq.capture(name)) {
          auto from_url = FromParam<P>::parse(*raw);
          if (!from_url)
            return std::unexpected(ApiError{404, "path.invalid", "not found",
                                            "capture {" + std::string(name) + "} = '" + std::string(*raw) +
                                                "' is not a valid value for body field '" + std::string(name) + "'"});
          if (field && !(*field == *from_url))
            return std::unexpected(ApiError::unprocessable(
                "json.path_mismatch", "field '" + std::string(name) + "' in the body does not match {" +
                                          std::string(name) + "} in the URL"));
          field = std::move(*from_url);
        } else if (field) {
          return std::unexpected(ApiError::unprocessable(
              "json.read_only", "field '" + std::string(name) + "' is assigned by the server and must not be sent"));
        }
      }
    }
  }
  return {};
}

}  // namespace detail

template <class T>
struct FromRequest<Json<T>> {
  static constexpr std::string_view kind = "json";
  static constexpr bool consumes_body = true;
  static std::expected<Json<T>, ApiError> extract(Request& r) {
    if (auto ct = r.header("content-type"); ct && !detail::is_json_media_type(*ct))
      return std::unexpected(ApiError{415, "json.unsupported_media_type",
                                      "expected content-type application/json", std::string(*ct)});
    if (r.body.empty())
      return std::unexpected(ApiError::unprocessable("json.invalid", "request body is empty; expected JSON"));
    auto doc = json::parse(r.body);
    if (!doc)
      return std::unexpected(ApiError::unprocessable(
          "json.invalid", "malformed JSON at line " + std::to_string(doc.error().line) + ", column " +
                              std::to_string(doc.error().column) + ": " + doc.error().message));
    Json<T> out{};
    if (auto ok = json::decode(*doc, out.value); !ok)
      return std::unexpected(ApiError::unprocessable("json.invalid", ok.error()));
    if (auto ok = detail::apply_from_path(r, out.value); !ok) return std::unexpected(std::move(ok.error()));
    return out;
  }
};

/// Authenticated principal. Requires a managed Authenticator.
struct Auth {
  std::string subject;
  std::vector<std::string> scopes = {};
  [[nodiscard]] bool has_scope(std::string_view s) const {
    for (auto& x : scopes)
      if (x == s) return true;
    return false;
  }
};

/// Verifies bearer tokens. Manage one: `.manage(Authenticator{verify_fn})`.
/// The verifier returns Auth, or an ApiError (conventionally 401 auth.expired
/// or auth.invalid). It runs on the worker thread and may block briefly.
class Authenticator {
 public:
  using Verify = std::function<std::expected<Auth, ApiError>(std::string_view token)>;
  explicit Authenticator(Verify v) : verify_(std::move(v)) {}
  [[nodiscard]] std::expected<Auth, ApiError> verify(std::string_view token) const { return verify_(token); }

 private:
  Verify verify_;
};

template <>
struct FromRequest<Auth> {
  static constexpr std::string_view kind = "auth";
  static constexpr bool consumes_body = false;
  static std::expected<Auth, ApiError> extract(Request& r) {
    auto h = r.header("authorization");
    if (!h || h->empty())
      return std::unexpected(ApiError::unauthorized("auth.missing", "missing bearer token"));
    constexpr std::string_view scheme = "bearer ";
    if (h->size() <= scheme.size() || !Headers::iequals(h->substr(0, scheme.size()), scheme))
      return std::unexpected(ApiError::unauthorized("auth.invalid", "authorization must use the Bearer scheme"));
    auto* verifier = r.state_registry ? r.state_registry->get<Authenticator>() : nullptr;
    if (!verifier) return std::unexpected(ApiError::internal("Auth used without a managed Authenticator"));
    return verifier->verify(h->substr(scheme.size()));
  }
  static void state_deps(std::vector<detail::StateDep>& out) { out.push_back(detail::state_dep<Authenticator>()); }
};

/// Deadline / cancellation for blocking work in handlers.
template <>
struct FromRequest<Deadline> {
  static constexpr std::string_view kind = "deadline";
  static constexpr bool consumes_body = false;
  static std::expected<Deadline, ApiError> extract(Request& r) { return r.deadline; }
};

/// The request id (from X-Request-Id, or generated).
struct RequestId {
  std::string value;
};
template <>
struct FromRequest<RequestId> {
  static constexpr std::string_view kind = "request_id";
  static constexpr bool consumes_body = false;
  static std::expected<RequestId, ApiError> extract(Request& r) { return RequestId{r.request_id}; }
};

/// Query<T>: maps query parameters onto the public members of aggregate T by
/// name. Members parse with FromParam; std::optional members may be absent.
/// Failures are 422 query.invalid.
template <class T>
struct Query {
  T value;
  T* operator->() { return &value; }
  const T* operator->() const { return &value; }
  T& operator*() { return value; }
};

template <class T>
  requires std::is_aggregate_v<T>
struct FromRequest<Query<T>> {
  static constexpr std::string_view kind = "query";
  static constexpr bool consumes_body = false;
  static std::expected<Query<T>, ApiError> extract(Request& r) {
    Query<T> out{};
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx))) {
      using M = [:std::meta::type_of(m):];
      constexpr std::string_view name = std::meta::identifier_of(m);
      auto raw = r.query_param(name);
      if constexpr (json::is_optional<M>::value) {
        using Inner = typename M::value_type;
        static_assert(PathParam<Inner>, "Query<T> member type must be parseable with FromParam");
        if (raw) {
          auto v = FromParam<Inner>::parse(*raw);
          if (!v) return bad(name);
          out.value.[:m:] = std::move(*v);
        }
      } else {
        static_assert(PathParam<M>, "Query<T> member type must be parseable with FromParam");
        if (!raw) {
          if constexpr (!std::meta::has_default_member_initializer(m))
            return std::unexpected(ApiError::unprocessable(
                "query.invalid", "missing query parameter '" + std::string(name) + "'"));
        } else {
          auto v = FromParam<M>::parse(*raw);
          if (!v) return bad(name);
          out.value.[:m:] = std::move(*v);
        }
      }
    }
    return out;
  }

 private:
  static std::unexpected<ApiError> bad(std::string_view name) {
    return std::unexpected(
        ApiError::unprocessable("query.invalid", "query parameter '" + std::string(name) + "' is not valid"));
  }
};

}  // namespace crocket
