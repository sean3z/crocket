// crocket in one file: routes declared with C++26 annotations, found by reflection.
// The real thing (h2o, typed extractors, JSON): https://github.com/sean3z/crocket
#include <meta>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <tuple>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace http {
// Annotation values must be structural, so the path is stored inline.
struct route {
  char method[8]{}, path[64]{};
  consteval route(std::string_view m, std::string_view p) {
    std::ranges::copy(m, method);
    std::ranges::copy(p, path);
  }
};
consteval route get(std::string_view p) { return {"GET", p}; }
}  // namespace http

namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  return std::format("Hello, {} year old named {}!", age, name);
}

[[= http::get("/")]]
auto index() -> std::string { return "Try GET /hello/<name>/<age>"; }

}  // namespace api

// ---- the framework part ------------------------------------------------------
auto split(std::string_view s) {
  std::vector<std::string_view> out;
  for (auto part : s | std::views::split('/'))
    if (!part.empty()) out.emplace_back(part.begin(), part.end());
  return out;
}

template <class T> auto parse(std::string_view s) -> std::optional<T> {
  if constexpr (std::is_same_v<T, std::string_view>) return s;
  else {
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
  }
}

// Call fn if the request path matches the pattern, converting each {param}.
template <std::meta::info Fn>
auto try_call(std::string_view pattern, std::string_view path) -> std::optional<std::string> {
  auto pat = split(pattern), got = split(path);
  if (pat.size() != got.size()) return std::nullopt;
  std::vector<std::string_view> args;
  for (std::size_t i = 0; i < pat.size(); ++i) {
    if (pat[i].starts_with('{')) args.push_back(got[i]);
    else if (pat[i] != got[i]) return std::nullopt;
  }
  constexpr std::size_t arity = std::meta::parameters_of(Fn).size();
  if (args.size() != arity) return std::nullopt;
  return [&args]<std::size_t... I>(std::index_sequence<I...>) -> std::optional<std::string> {
    std::tuple vals{parse<typename[:std::meta::remove_cvref(std::meta::type_of(std::meta::parameters_of(Fn)[I])):]>(args[I])...};
    if (!(std::get<I>(vals) && ...)) return std::nullopt;  // bad param: 404, handler never runs
    return [:Fn:](*std::get<I>(vals)...);
  }(std::make_index_sequence<arity>{});
}

// libstdc++ calls this annotations_of_with_type; clang-p2996's libc++ overloads annotations_of.
consteval auto route_annotations(std::meta::info fn) {
#if defined(__clang__)
  return std::meta::annotations_of(fn, ^^http::route);
#else
  return std::meta::annotations_of_with_type(fn, ^^http::route);
#endif
}

template <std::meta::info Ns>
auto dispatch(std::string_view method, std::string_view path) -> std::string {
  constexpr auto ctx = std::meta::access_context::current();
  template for (constexpr auto fn : std::define_static_array(std::meta::members_of(Ns, ctx))) {
    if constexpr (std::meta::is_function(fn)) {
      template for (constexpr auto a : std::define_static_array(route_annotations(fn))) {
        constexpr auto r = std::meta::extract<http::route>(a);
        if (method == r.method)
          if (auto body = try_call<fn>(r.path, path)) return "200 " + *body;
      }
    }
  }
  return "404 Not Found";
}

int main() {
  for (auto path : {"/", "/hello/Rocketeer/42", "/hello/Rocketeer/400", "/nope"})
    std::println("GET {:22} -> {}", path, dispatch<^^api>("GET", path));
}
