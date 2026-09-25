#pragma once
// reflect_routes<^^scope>(): the only place reflection touches routing.
//
// For every function in `scope` (a namespace, or a class = controller) that
// carries exactly one http::Route annotation, it:
//   * binds each `{capture}` to the parameter of the same name,
//   * treats every other parameter as an extractor,
//   * checks the return type is a Responder (or an awaitable of one),
// and fails compilation with a message naming the handler and identifiers
// otherwise. The result is a table of RouteDefs whose invokers are plain
// function pointers: no reflection runs on the request path.
//
// Controllers: static member functions are ordinary handlers; non-static
// member functions run on the managed instance (`.manage(Controller{...})`),
// which ignite requires.

#include "crocket/app.hpp"

#include <meta>
#include <ranges>
#include <string>
#include <vector>

namespace crocket {
namespace detail::reflect {

template <class T>
inline constexpr bool path_param_v = PathParam<T>;
template <class T>
inline constexpr bool extractor_v = Extractor<T>;
template <class T>
inline constexpr bool body_consumer_v = [] {
  if constexpr (Extractor<T>) return bool(FromRequest<T>::consumes_body);
  else return false;
}();
template <class R>
inline constexpr bool valid_result_v = [] {
  using U = std::remove_cvref_t<R>;
  if constexpr (std::is_void_v<U>) return true;
  else if constexpr (Awaitable<U>) {
    using V = await_result_t<U>;
    return std::is_void_v<V> || IsResponder<std::remove_cvref_t<V>>;
  } else return IsResponder<U>;
}();

consteval bool holds(std::meta::info trait, std::meta::info type) {
  return std::meta::extract<bool>(std::meta::substitute(trait, {type}));
}

consteval std::string name_of(std::meta::info r) {
  return std::meta::has_identifier(r) ? std::string(std::meta::identifier_of(r)) : std::string("(anonymous)");
}

consteval std::string qualified_name(std::meta::info r) {
  std::string name = name_of(r);
  for (auto p = std::meta::parent_of(r); p != ^^::; p = std::meta::parent_of(p)) name = name_of(p) + "::" + name;
  return name;
}

consteval std::string type_name(std::meta::info t) { return std::string(std::meta::display_string_of(t)); }

consteval std::vector<std::meta::info> route_functions(std::meta::info scope) {
  std::vector<std::meta::info> out;
  for (auto m : std::meta::members_of(scope, std::meta::access_context::current()))
    if (std::meta::is_function(m) && !std::meta::annotations_of_with_type(m, ^^http::Route).empty())
      out.push_back(m);
  return out;
}

consteval http::Route route_of(std::meta::info fn) {
  return std::meta::extract<http::Route>(std::meta::annotations_of_with_type(fn, ^^http::Route)[0]);
}

/// Computes the parameter bindings for `fn`, or returns a diagnostic.
consteval std::string analyze(std::meta::info fn, std::vector<Binding>& out) {
  out.clear();
  auto anns = std::meta::annotations_of_with_type(fn, ^^http::Route);
  std::string where = "crocket: handler " + qualified_name(fn);
  if (anns.size() != 1)
    return where + " has " + std::to_string(anns.size()) +
           " route annotations; one route per function (use two functions for two verbs)";

  auto route = std::meta::extract<http::Route>(anns[0]);
  where += std::string(" [") + std::string(http::method_name(route.method)) + " " + route.path + "]";

  std::vector<http::Segment> segs;
  (void)http::parse_template(route.path, segs);  // validated in the annotation
  std::vector<std::string_view> captures;
  for (auto& s : segs)
    if (s.capture) captures.push_back(s.text);

  auto params = std::meta::parameters_of(fn);
  std::vector<bool> capture_used(captures.size(), false);
  std::vector<std::string> unbound_pathlike;
  std::string all_params;
  int body_consumers = 0;

  for (std::size_t i = 0; i < params.size(); ++i) {
    auto p = params[i];
    if (!std::meta::has_identifier(p))
      return where + ": parameter #" + std::to_string(i + 1) +
             " has no name (or different names across declarations); handler parameters must be named";
    std::string_view pname = std::meta::identifier_of(p);
    auto t = std::meta::type_of(p);
    auto u = std::meta::remove_cvref(t);
    if (!all_params.empty()) all_params += ", ";
    all_params += pname;

    int cap = -1;
    for (std::size_t c = 0; c < captures.size(); ++c)
      if (captures[c] == pname) cap = int(c);

    if (cap >= 0) {
      if (!holds(^^path_param_v, u))
        return where + ": parameter '" + std::string(pname) + "' binds to path capture '{" + std::string(pname) +
               "}' but its type '" + type_name(t) + "' cannot be parsed from a path segment (no FromParam<" +
               type_name(u) + ">)";
      capture_used[cap] = true;
      out.push_back({BindKind::Path, cap, std::define_static_string(pname)});
    } else if (u == (^^Request) && std::meta::is_lvalue_reference_type(t)) {
      out.push_back({BindKind::RawRequest, -1, std::define_static_string(pname)});
    } else if (u == (^^Response) && std::meta::is_lvalue_reference_type(t) &&
               !std::meta::is_const(std::meta::remove_reference(t))) {
      out.push_back({BindKind::RawResponse, -1, std::define_static_string(pname)});
    } else if (holds(^^extractor_v, u)) {
      if (holds(^^body_consumer_v, u)) ++body_consumers;
      out.push_back({BindKind::Extract, -1, std::define_static_string(pname)});
    } else if (holds(^^path_param_v, u)) {
      unbound_pathlike.push_back(std::string(pname));
      out.push_back({BindKind::Path, -1, std::define_static_string(pname)});
    } else {
      return where + ": parameter '" + std::string(pname) + "' of type '" + type_name(t) +
             "' is not a path capture and not an extractor (no FromRequest<" + type_name(u) + ">)";
    }
  }

  for (std::size_t c = 0; c < captures.size(); ++c) {
    if (capture_used[c]) continue;
    std::string msg = where + ": path capture '{" + std::string(captures[c]) + "}' has no parameter named '" +
                      std::string(captures[c]) + "'";
    if (!unbound_pathlike.empty()) msg += "; did you mean parameter '" + unbound_pathlike.front() + "'?";
    msg += " (parameters: " + (all_params.empty() ? std::string("none") : all_params) + ")";
    return msg;
  }
  if (!unbound_pathlike.empty())
    return where + ": parameter '" + unbound_pathlike.front() + "' is not in the path (no '{" +
           unbound_pathlike.front() + "}') and its type is not an extractor";
  if (body_consumers > 1)
    return where + ": more than one parameter consumes the request body";

  auto ret = std::meta::return_type_of(fn);
  if (!holds(^^valid_result_v, ret))
    return where + ": return type '" + type_name(ret) +
           "' is not a Responder (use std::string, Json<T>, Created<T>, std::optional<T>, "
           "std::expected<T, ApiError>, ... or specialize crocket::Responder)";
  return {};
}

consteval std::string diagnose(std::meta::info fn) {
  std::vector<Binding> b;
  return analyze(fn, b);
}

consteval std::vector<Binding> bindings_for(std::meta::info fn) {
  std::vector<Binding> b;
  if (!analyze(fn, b).empty()) b.clear();
  return b;
}

template <class T>
void describe_param(RouteDef& d, std::string_view name, std::string_view type, bool is_path) {
  using U = std::remove_cvref_t<T>;
  if (is_path) {
    d.params.push_back({name, "path", type});
  } else if constexpr (std::is_same_v<U, Request>) {
    d.params.push_back({name, "request", type});
  } else if constexpr (std::is_same_v<U, Response>) {
    d.params.push_back({name, "response", type});
  } else if constexpr (Extractor<U>) {
    d.params.push_back({name, FromRequest<U>::kind, type});
    collect_state_deps<U>(d.needs);
  }
}

}  // namespace detail::reflect

/// Collect the annotated handlers of a namespace or class.
template <std::meta::info Scope>
Routes reflect_routes() {
  static_assert(std::meta::is_namespace(Scope) || std::meta::is_class_type(Scope),
                "reflect_routes<^^X>(): X must be a namespace or a class");
  namespace r = detail::reflect;
  Routes out;
  template for (constexpr auto fn : std::define_static_array(r::route_functions(Scope))) {
    constexpr std::string_view problem = std::define_static_string(r::diagnose(fn));
    static_assert(problem.empty(), problem);
    if constexpr (problem.empty()) {
      static constexpr auto bindings = std::define_static_array(r::bindings_for(fn));
      constexpr http::Route route = r::route_of(fn);
      RouteDef d;
      d.method = route.method;
      d.path = route.path;
      d.rank = route.rank;
      d.handler = std::define_static_string(r::qualified_name(fn));
      constexpr auto params = std::define_static_array(std::meta::parameters_of(fn));
      template for (constexpr std::size_t i : std::define_static_array(std::views::iota(std::size_t{0}, params.size()))) {
        constexpr auto p = params[i];
        using T = [:std::meta::type_of(p):];
        r::describe_param<T>(d, std::meta::identifier_of(p),
                             std::define_static_string(std::meta::display_string_of(std::meta::type_of(p))),
                             bindings[i].kind == detail::BindKind::Path);
      }
      if constexpr (std::meta::is_class_member(fn) && !std::meta::is_static_member(fn)) {
        using C = [:std::meta::parent_of(fn):];
        d.needs.push_back(detail::state_dep<C>());
      }
      d.invoke = &detail::invoke<&[:fn:], bindings.data()>;
      out.push_back(std::move(d));
    }
  }
  return out;
}

}  // namespace crocket
