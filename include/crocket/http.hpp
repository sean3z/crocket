#pragma once
// HTTP vocabulary shared by handlers, the router, and the engine.
// Nothing in this header knows about the wire engine.

#include <meta>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crocket {

namespace detail {
/// std::to_string for compile-time diagnostics (libstdc++'s is not constexpr).
constexpr std::string decimal(long long v) {
  if (v == 0) return "0";
  bool neg = v < 0;
  unsigned long long u = neg ? 0ULL - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
  std::string s;
  for (; u; u /= 10) s.insert(s.begin(), char('0' + u % 10));
  return neg ? "-" + s : s;
}
}  // namespace detail

namespace http {

enum class Method : std::uint8_t { Get, Head, Post, Put, Patch, Delete, Options, Unknown };

constexpr std::string_view method_name(Method m) noexcept {
  switch (m) {
    case Method::Get: return "GET";
    case Method::Head: return "HEAD";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Patch: return "PATCH";
    case Method::Delete: return "DELETE";
    case Method::Options: return "OPTIONS";
    default: return "UNKNOWN";
  }
}

constexpr Method parse_method(std::string_view s) noexcept {
  for (auto m : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Patch,
                 Method::Delete, Method::Options})
    if (method_name(m) == s) return m;
  return Method::Unknown;
}

/// A header name is an RFC 9110 token: letters, digits and !#$%&'*+-.^_`|~.
constexpr bool valid_header_name(std::string_view name) noexcept {
  if (name.empty()) return false;
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    if (!ok) return false;
  }
  return true;
}

/// A header value must not contain CR, LF or NUL: they would end the header and
/// let the rest of the value inject new headers (response splitting).
constexpr bool valid_header_value(std::string_view value) noexcept {
  return value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}

/// One path segment of a route template: either literal text or a `{capture}`.
struct Segment {
  bool capture;
  std::string_view text;  // literal text, or the capture's identifier
};

/// Parses "/users/{id}/posts". Usable at compile time (annotation validation,
/// capture/parameter binding) and at run time (router construction).
/// Returns an empty string on success, otherwise a description of the problem.
constexpr std::string parse_template(std::string_view path, std::vector<Segment>& out) {
  out.clear();
  if (path.empty() || path.front() != '/')
    return "route path must start with '/'";
  if (path == "/") return {};
  std::string_view rest = path.substr(1);
  while (true) {
    auto slash = rest.find('/');
    std::string_view seg = rest.substr(0, slash);
    if (seg.empty()) return "route path has an empty segment ('//' or a trailing '/')";
    if (seg.front() == '{') {
      if (seg.back() != '}' || seg.size() < 3)
        return std::string("capture '") + std::string(seg) +
               "' must be a whole segment of the form {name}";
      auto id = seg.substr(1, seg.size() - 2);
      auto ident_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
      };
      if (!std::ranges::all_of(id, ident_char) || (id.front() >= '0' && id.front() <= '9'))
        return std::string("capture '{") + std::string(id) + "}' is not a valid identifier";
      for (auto& s : out)
        if (s.capture && s.text == id)
          return std::string("capture '{") + std::string(id) + "}' appears twice";
      out.push_back({true, id});
    } else {
      if (seg.find_first_of("{}") != std::string_view::npos)
        return std::string("segment '") + std::string(seg) +
               "' mixes literal text and a capture; captures must be whole segments";
      out.push_back({false, seg});
    }
    if (slash == std::string_view::npos) break;
    rest = rest.substr(slash + 1);
  }
  return {};
}

/// Per-route options, e.g. `http::get("/users/{id}", {.rank = 2})`.
/// Lower rank is tried first when two templates match the same path; a route
/// whose path capture fails to parse forwards to the next candidate.
struct RouteOptions {
  int rank = 0;
};

/// The route annotation. Structural so it can live in `[[= ...]]`.
struct Route {
  Method method;
  const char* path;  // static storage (std::define_static_string)
  int rank;
};

consteval Route route(Method m, std::string_view path, RouteOptions opts = {}) {
  std::vector<Segment> segs;
  if (auto err = parse_template(path, segs); !err.empty()) {
    std::string msg = "crocket: invalid route \"" + std::string(path) + "\": " + err;
    throw std::meta::exception(std::u8string(msg.begin(), msg.end()), ^^Route);
  }
  return Route{m, std::define_static_string(path), opts.rank};
}

consteval Route get(std::string_view p, RouteOptions o = {}) { return route(Method::Get, p, o); }
consteval Route head(std::string_view p, RouteOptions o = {}) { return route(Method::Head, p, o); }
consteval Route post(std::string_view p, RouteOptions o = {}) { return route(Method::Post, p, o); }
consteval Route put(std::string_view p, RouteOptions o = {}) { return route(Method::Put, p, o); }
consteval Route patch(std::string_view p, RouteOptions o = {}) { return route(Method::Patch, p, o); }
consteval Route del(std::string_view p, RouteOptions o = {}) { return route(Method::Delete, p, o); }
consteval Route options(std::string_view p, RouteOptions o = {}) { return route(Method::Options, p, o); }

/// An HTTP date (RFC 9110 IMF-fixdate), "Sun, 06 Nov 1994 08:49:37 GMT", for
/// headers such as Last-Modified.
std::string date(std::chrono::system_clock::time_point t);
/// Parses an IMF-fixdate; nullopt for anything else.
std::optional<std::chrono::sys_seconds> parse_date(std::string_view s);

}  // namespace http

/// Header list with lowercase names (HTTP/2 requires lowercase; HTTP/1.1 is
/// case-insensitive). Order is preserved; duplicates are allowed via add().
class Headers {
 public:
  using value_type = std::pair<std::string, std::string>;

  void add(std::string_view name, std::string_view value) {
    reserve();
    items_.emplace_back(lower(name), std::string(value));
  }
  /// Replaces every field named `name` with one, or adds it.
  void set(std::string_view name, std::string_view value) {
    auto it = std::ranges::find_if(items_, [&](auto& kv) { return iequals(kv.first, name); });
    if (it == items_.end()) return add(name, value);
    it->second.assign(value);
    items_.erase(std::remove_if(it + 1, items_.end(), [&](auto& kv) { return iequals(kv.first, name); }), items_.end());
  }
  void remove(std::string_view name) {
    auto n = lower(name);
    std::erase_if(items_, [&](auto& kv) { return kv.first == n; });
  }
  [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const {
    for (auto& [k, v] : items_)
      if (iequals(k, name)) return std::string_view(v);
    return std::nullopt;
  }
  [[nodiscard]] bool contains(std::string_view name) const { return get(name).has_value(); }
  [[nodiscard]] auto begin() const { return items_.begin(); }
  [[nodiscard]] auto end() const { return items_.end(); }
  [[nodiscard]] std::size_t size() const { return items_.size(); }
  [[nodiscard]] bool empty() const { return items_.empty(); }

  static bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
  }

 private:
  static constexpr char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
  // Most messages carry a handful of fields: no regrowth for the first 8.
  void reserve() {
    if (items_.capacity() == 0) items_.reserve(8);
  }
  static std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = ascii_lower(c);
    return r;
  }
  std::vector<value_type> items_;
};

}  // namespace crocket
