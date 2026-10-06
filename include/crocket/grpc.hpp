#pragma once
// Unary gRPC on the same engine, routes and extractors as HTTP. Messages are
// plain structs encoded with crocket::proto, so the C++ declarations are the
// schema; proto_file<^^ns>() prints the matching .proto for other languages.
//
//   namespace greeter {
//   [[= grpc::rpc]]
//   auto say_hello(HelloRequest req, Auth auth) -> Result<HelloReply> { ... }
//   }
//
//   crocket::build().mount("helloworld", reflect_routes<^^greeter>(), Mode::Grpc);
//   // serves POST /helloworld.Greeter/SayHello
//
// The service is the namespace or class and the method the function, both in
// PascalCase. The mount's first argument is the protobuf package.
//
// A method takes at most one request message (any aggregate struct that is
// not an extractor; none means google.protobuf.Empty) plus any extractors that
// do not read the body. It returns a message, Result<T> (std::expected<T,
// ApiError>), std::optional<T> (empty: NOT_FOUND), void (Empty), or a Task of
// one of those. An ApiError becomes a gRPC status: see code_of(), or build one
// with an exact code using grpc::error().

#include "crocket/detail/invoke.hpp"
#include "crocket/proto.hpp"

#include <meta>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace crocket::grpc {

/// The method annotation: `[[= grpc::rpc]]`.
struct Rpc {
  constexpr bool operator==(const Rpc&) const = default;
};
inline constexpr Rpc rpc{};

/// gRPC status codes.
enum class Code : std::uint8_t {
  Ok = 0,
  Cancelled = 1,
  Unknown = 2,
  InvalidArgument = 3,
  DeadlineExceeded = 4,
  NotFound = 5,
  AlreadyExists = 6,
  PermissionDenied = 7,
  ResourceExhausted = 8,
  FailedPrecondition = 9,
  Aborted = 10,
  OutOfRange = 11,
  Unimplemented = 12,
  Internal = 13,
  Unavailable = 14,
  DataLoss = 15,
  Unauthenticated = 16,
};

/// The HTTP status conventionally paired with a gRPC code (google.rpc.Code).
int http_status(Code c);

/// The gRPC code for an error: ApiError::grpc_status when set, else mapped from
/// the HTTP status (400/422 InvalidArgument, 401 Unauthenticated, 403
/// PermissionDenied, 404 NotFound, 409 AlreadyExists, 412 FailedPrecondition,
/// 413/429 ResourceExhausted, 499 Cancelled, 501 Unimplemented, 503
/// Unavailable, 504 DeadlineExceeded, other 5xx Internal).
Code code_of(const ApiError& e);

/// An ApiError with an exact gRPC code (and the matching HTTP status, so the
/// same error reads sensibly from an HTTP route).
inline ApiError error(Code c, std::string_view code, std::string message) {
  return ApiError{http_status(c), code, std::move(message), {}, static_cast<int>(c)};
}

/// "application/grpc", "application/grpc+proto", "application/grpc; ...".
/// Not grpc-web, which is a different protocol.
bool is_grpc_content_type(std::string_view ct);
bool is_grpc_request(const Request& rq);

/// One length-prefixed message: flag byte 0 (uncompressed), big-endian length, bytes.
std::string frame(std::string_view message);

/// The outcome of a call, as a client sees it.
struct Status {
  Code code = Code::Ok;
  std::string message = {};
  [[nodiscard]] bool ok() const { return code == Code::Ok; }
};

/// Reads grpc-status / grpc-message from a response (trailers, or headers for
/// a trailers-only response).
Status status_of(const Response& rs);

namespace detail {

/// The single message in a unary request body.
std::expected<std::string_view, ApiError> unframe(const Request& rq);
std::expected<std::string_view, Status> unframe_reply(const Response& rs);

/// A successful reply: status 200, application/grpc, the framed message, and
/// grpc-status 0 in the trailers.
void write_reply(std::string_view message, Response& rs);

/// A failed call as a trailers-only response. write_error() calls this for
/// gRPC requests.
void write_status(const ApiError& e, const Request& rq, Response& rs);

}  // namespace detail

/// Decodes a reply message, or returns the call's failure status.
template <proto::Message T>
std::expected<T, Status> read_reply(const Response& rs) {
  auto st = status_of(rs);
  if (!st.ok()) return std::unexpected(std::move(st));
  auto payload = detail::unframe_reply(rs);
  if (!payload) return std::unexpected(std::move(payload.error()));
  auto v = proto::decode<T>(*payload);
  if (!v) return std::unexpected(Status{Code::Internal, std::move(v.error())});
  return std::move(*v);
}

// ---- handler binding --------------------------------------------------------------

namespace detail {

using crocket::detail::Binding;
using crocket::detail::BindKind;
using crocket::detail::Outcome;
using crocket::detail::Slot;
using crocket::detail::type_list;

/// The request message parameter: an aggregate that is not an extractor.
template <class T>
inline constexpr bool request_message_v = [] {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, Request> || std::is_same_v<U, Response> || Extractor<U>) return false;
  else return proto::Message<U>;
}();

/// What a handler's return type R replies with. `type` is the message (void
/// for google.protobuf.Empty); `valid` is false for unsupported returns.
template <class R>
struct reply_of {
  static constexpr bool valid = proto::Message<R>;
  using type = R;
};
template <>
struct reply_of<void> {
  static constexpr bool valid = true;
  using type = void;
};
template <class T>
struct reply_of<std::optional<T>> {
  static constexpr bool valid = proto::Message<T>;
  using type = T;
};
template <class T>
struct reply_of<std::expected<T, ApiError>> {
  static constexpr bool valid = std::is_void_v<T> || proto::Message<T>;
  using type = T;
};
template <class R, bool = crocket::detail::Awaitable<std::remove_cvref_t<R>>>
struct reply : reply_of<std::remove_cvref_t<R>> {};
template <class R>
struct reply<R, true> : reply_of<std::remove_cvref_t<crocket::detail::await_result_t<std::remove_cvref_t<R>>>> {};

template <class R>
inline constexpr bool valid_reply_v = reply<R>::valid;

template <class T>
void reply_value(T&& v, const Request& rq, Response& rs) {
  using U = std::remove_cvref_t<T>;
  if constexpr (json::is_optional<U>::value) {
    if (v) reply_value(std::move(*v), rq, rs);
    else write_error(ApiError::not_found(), rq, rs);
  } else if constexpr (requires { typename U::error_type; }) {
    if (!v) write_error(v.error(), rq, rs);
    else if constexpr (std::is_void_v<typename U::value_type>) write_reply({}, rs);
    else reply_value(std::move(*v), rq, rs);
  } else {
    write_reply(proto::encode(v), rs);
  }
}

template <class A, Binding B>
bool fill(Request& rq, Response& rs, std::string_view payload, Slot<A>& slot, Outcome& out) {
  if constexpr (B.kind == BindKind::Message) {
    auto v = proto::decode<std::remove_cvref_t<A>>(payload);
    if (!v) {
      out.kind = Outcome::Failed;
      out.failure_kind = "proto";
      out.error = ApiError::bad_request("proto.invalid", std::move(v.error()));
      return false;
    }
    slot.emplace(std::move(*v));
    return true;
  } else {
    return crocket::detail::fill<A, B>(rq, rs, slot, out);
  }
}

template <auto Fn, const Binding* B, class... A, std::size_t... I>
Outcome invoke_impl(Request& rq, Response& rs, type_list<A...>, std::index_sequence<I...>) {
  using Traits = crocket::detail::fn_traits<decltype(Fn)>;
  using R = typename Traits::ret;
  using C = typename Traits::cls;

  Outcome out;
  auto payload = unframe(rq);
  if (!payload) {
    out.kind = Outcome::Failed;
    out.failure_kind = "grpc";
    out.error = std::move(payload.error());
    return out;
  }
  std::tuple<Slot<A>...> slots;
  if (!(grpc::detail::fill<A, B[I]>(rq, rs, *payload, std::get<I>(slots), out) && ...)) return out;

  auto call = [&]() -> R {
    if constexpr (std::is_void_v<C>) {
      return Fn(crocket::detail::pass<A>(std::get<I>(slots))...);
    } else {
      C* self = rq.state_registry->template get<C>();
      return (self->*Fn)(crocket::detail::pass<A>(std::get<I>(slots))...);
    }
  };

  if constexpr (std::is_void_v<R>) {
    call();
    write_reply({}, rs);
  } else if constexpr (crocket::detail::Awaitable<std::remove_cvref_t<R>>) {
    if constexpr (std::is_void_v<crocket::detail::await_result_t<std::remove_cvref_t<R>>>)
      return crocket::detail::start_async(call(), rq, rs, [&rs] { write_reply({}, rs); });
    else
      return crocket::detail::start_async(call(), rq, rs,
                                          [&rq, &rs](auto&& v) { reply_value(std::forward<decltype(v)>(v), rq, rs); });
  } else {
    reply_value(call(), rq, rs);
  }
  return out;
}

template <auto Fn, const Binding* B>
Outcome invoke(Request& rq, Response& rs) {
  using Traits = crocket::detail::fn_traits<decltype(Fn)>;
  return grpc::detail::invoke_impl<Fn, B>(rq, rs, typename Traits::args{}, std::make_index_sequence<Traits::arity>{});
}

// ---- schema -----------------------------------------------------------------------

consteval bool is_rpc(std::meta::info fn) {
  return std::meta::is_function(fn) && !std::meta::annotations_of_with_type(fn, ^^Rpc).empty();
}

consteval std::vector<std::meta::info> rpcs_of(std::meta::info scope) {
  std::vector<std::meta::info> out;
  for (auto m : std::meta::members_of(scope, std::meta::access_context::current()))
    if (is_rpc(m)) out.push_back(m);
  return out;
}

/// "greeter" -> "Greeter"; the service a namespace or class serves.
consteval std::string_view service_name(std::meta::info scope) {
  return std::define_static_string(
      std::meta::has_identifier(scope) ? proto::detail::pascal_case(std::meta::identifier_of(scope)) : "");
}
consteval std::string_view method_name(std::meta::info fn) {
  return std::define_static_string(proto::detail::pascal_case(std::meta::identifier_of(fn)));
}

template <class List>
struct request_of {
  using type = void;
};
template <class A, class... Rest>
struct request_of<type_list<A, Rest...>> {
  using type = std::conditional_t<request_message_v<A>, std::remove_cvref_t<A>,
                                  typename request_of<type_list<Rest...>>::type>;
};

template <class T>
std::string message_ref(proto::Schema& s, bool& uses_empty) {
  if constexpr (std::is_void_v<T>) {
    uses_empty = true;
    return "google.protobuf.Empty";
  } else {
    return s.message<T>();
  }
}

template <std::meta::info Scope>
void describe_service(proto::Schema& s, std::string& out, bool& uses_empty) {
  static_assert(std::meta::has_identifier(Scope), "crocket: gRPC services need a named namespace or class");
  out += "service " + std::string(service_name(Scope)) + " {\n";
  template for (constexpr auto fn : std::define_static_array(rpcs_of(Scope))) {
    using Traits = crocket::detail::fn_traits<decltype(&[:fn:])>;
    using Req = typename request_of<typename Traits::args>::type;
    using Rep = typename reply<typename Traits::ret>::type;
    auto req = message_ref<Req>(s, uses_empty);
    auto rep = message_ref<Rep>(s, uses_empty);
    out += "  rpc " + std::string(method_name(fn)) + " (" + req + ") returns (" + rep + ");\n";
  }
  out += "}\n";
}

}  // namespace detail

/// The .proto source for the [[= grpc::rpc]] methods of each scope and every
/// message they use, in package `package` (pass the same string as mount()).
/// Throws std::logic_error when two C++ types would share a proto name.
template <std::meta::info... Scopes>
std::string proto_file(std::string_view package) {
  proto::Schema schema;
  std::string services;
  bool uses_empty = false;
  ((services += services.empty() ? "" : "\n", detail::describe_service<Scopes>(schema, services, uses_empty)), ...);
  if (!schema.conflicts().empty()) throw std::logic_error("crocket: " + schema.conflicts().front());
  std::string out = "// Generated by crocket from C++ declarations.\nsyntax = \"proto3\";\n";
  if (!package.empty()) out += "\npackage " + std::string(package) + ";\n";
  if (uses_empty) out += "\nimport \"google/protobuf/empty.proto\";\n";
  out += "\n" + services + schema.definitions();
  return out;
}

}  // namespace crocket::grpc
