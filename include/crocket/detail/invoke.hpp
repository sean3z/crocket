#pragma once
// Turns a handler function pointer plus a compile-time binding table into a
// uniform `Outcome(Request&, Response&)`. reflect_routes() produces the
// bindings; everything here is ordinary C++20 templates.

#include "crocket/extract.hpp"
#include "crocket/responder.hpp"
#include "crocket/task.hpp"

#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace crocket::detail {

enum class BindKind : std::uint8_t { Path, Extract, RawRequest, RawResponse, Message };  // Message: gRPC only

/// How one handler parameter gets its value. Structural (lives in an NTTP).
struct Binding {
  BindKind kind;
  int capture;       // index into the route's captures when kind == Path
  const char* name;  // parameter identifier, for diagnostics
};

struct Outcome {
  // Async: the handler returned a task that suspended; it fills the response
  // and finishes the request itself (see Exchange).
  enum Kind : std::uint8_t { Done, Forward, Failed, Async };
  Kind kind = Done;
  ApiError error = {};              // when Failed
  std::string_view failure_kind;    // "path", "json", "auth", ...
  std::string detail;               // when Forward: which capture failed
};

using Invoker = Outcome (*)(Request&, Response&);

template <class... A>
struct type_list {};

template <class F>
struct fn_traits;
template <class R, class... A>
struct fn_traits<R (*)(A...)> {
  using ret = R;
  using args = type_list<A...>;
  using cls = void;
  static constexpr std::size_t arity = sizeof...(A);
};
template <class R, class... A>
struct fn_traits<R (*)(A...) noexcept> : fn_traits<R (*)(A...)> {};
template <class R, class C, class... A>
struct fn_traits<R (C::*)(A...)> {
  using ret = R;
  using args = type_list<A...>;
  using cls = C;
  static constexpr std::size_t arity = sizeof...(A);
};
template <class R, class C, class... A>
struct fn_traits<R (C::*)(A...) const> : fn_traits<R (C::*)(A...)> {};
template <class R, class C, class... A>
struct fn_traits<R (C::*)(A...) noexcept> : fn_traits<R (C::*)(A...)> {};
template <class R, class C, class... A>
struct fn_traits<R (C::*)(A...) const noexcept> : fn_traits<R (C::*)(A...)> {};

template <class A>
inline constexpr bool is_raw_request = std::is_same_v<std::remove_cvref_t<A>, Request> && std::is_lvalue_reference_v<A>;
template <class A>
inline constexpr bool is_raw_response = std::is_same_v<std::remove_cvref_t<A>, Response> && std::is_lvalue_reference_v<A>;

template <class A>
using Slot = std::conditional_t<is_raw_request<A>, Request*,
                                std::conditional_t<is_raw_response<A>, Response*, std::optional<std::remove_cvref_t<A>>>>;

template <class A, Binding B>
bool fill(Request& rq, Response& rs, Slot<A>& slot, Outcome& out) {
  using T = std::remove_cvref_t<A>;
  if constexpr (B.kind == BindKind::RawRequest) {
    slot = &rq;
  } else if constexpr (B.kind == BindKind::RawResponse) {
    slot = &rs;
  } else if constexpr (B.kind == BindKind::Path) {
    auto v = FromParam<T>::parse(rq.captures[B.capture]);
    if (!v) {
      out.kind = Outcome::Forward;
      out.failure_kind = "path";
      out.detail = std::string("capture '{") + B.name + "}' did not parse";
      return false;
    }
    slot.emplace(std::move(*v));
  } else {
    auto v = FromRequest<T>::extract(rq);
    if (!v) {
      out.kind = Outcome::Failed;
      out.failure_kind = FromRequest<T>::kind;
      out.error = std::move(v.error());
      return false;
    }
    slot.emplace(std::move(*v));
  }
  return true;
}

template <class A>
decltype(auto) pass(Slot<A>& slot) {
  if constexpr (is_raw_request<A> || is_raw_response<A>) return (*slot);
  else if constexpr (std::is_lvalue_reference_v<A>) return (*slot);
  else return std::move(*slot);
}

/// An exception from a handler as the response it becomes (500, or the ApiError thrown).
void respond_exception(std::exception_ptr e, Request& rq, Response& rs);
/// The async handler of rq has finished filling the response.
void async_handler_done(Request& rq);

/// A coroutine that starts when called and frees itself at the end.
struct Detached {
  struct promise_type {
    Detached get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };
};

/// Runs an async handler's result to completion, then turns its value into the
/// response with `on_value`. Runs inline until the task first suspends.
template <class Aw, class OnValue>
Detached drive(Aw aw, Request& rq, Response& rs, OnValue on_value) {
  try {
    if constexpr (std::is_void_v<await_result_t<Aw>>) {
      co_await worker_await(std::move(aw));
      on_value();
    } else {
      on_value(co_await worker_await(std::move(aw)));
    }
  } catch (...) {
    respond_exception(std::current_exception(), rq, rs);
  }
  async_handler_done(rq);
}

/// Starts an awaitable handler result; the request finishes when it does.
template <class R, class OnValue>
Outcome start_async(R&& r, Request& rq, Response& rs, OnValue on_value) {
  drive(std::forward<R>(r), rq, rs, std::move(on_value));
  Outcome out;
  out.kind = Outcome::Async;
  return out;
}

template <class R>
Outcome respond_result(R&& r, Request& rq, Response& rs) {
  using U = std::remove_cvref_t<R>;
  if constexpr (Awaitable<U>) {
    if constexpr (std::is_void_v<await_result_t<U>>)
      return start_async(std::forward<R>(r), rq, rs, [&rq, &rs] { Responder<NoContent>::respond(NoContent{}, rq, rs); });
    else
      return start_async(std::forward<R>(r), rq, rs,
                         [&rq, &rs](auto&& v) { respond_value(std::forward<decltype(v)>(v), rq, rs); });
  } else {
    respond_value(std::forward<R>(r), rq, rs);
    return {};
  }
}

template <auto Fn, const Binding* B, class... A, std::size_t... I>
Outcome invoke_impl(Request& rq, Response& rs, type_list<A...>, std::index_sequence<I...>) {
  using Traits = fn_traits<decltype(Fn)>;
  using R = typename Traits::ret;
  using C = typename Traits::cls;

  std::tuple<Slot<A>...> slots;
  Outcome out;
  // Left-to-right, stopping at the first failure: a failed extractor means
  // later extractors (and the handler) do not run.
  if (!(fill<A, B[I]>(rq, rs, std::get<I>(slots), out) && ...)) return out;

  auto call = [&]() -> R {
    if constexpr (std::is_void_v<C>) {
      return Fn(pass<A>(std::get<I>(slots))...);
    } else {
      // Controller instance comes from managed state (checked at ignite).
      C* self = rq.state_registry->template get<C>();
      return (self->*Fn)(pass<A>(std::get<I>(slots))...);
    }
  };

  if constexpr (std::is_void_v<R>) {
    call();
    // A void handler that wrote to Response& keeps what it wrote.
    if constexpr (!(is_raw_response<A> || ...)) Responder<NoContent>::respond(NoContent{}, rq, rs);
  } else {
    return respond_result(call(), rq, rs);
  }
  return out;
}

template <auto Fn, const Binding* B>
Outcome invoke(Request& rq, Response& rs) {
  using Traits = fn_traits<decltype(Fn)>;
  return invoke_impl<Fn, B>(rq, rs, typename Traits::args{}, std::make_index_sequence<Traits::arity>{});
}

}  // namespace crocket::detail
