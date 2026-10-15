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
      // Request paths are matched after normalize_path(): no dot segments, and
      // '%' kept only as an escape.
      if (seg == "." || seg == "..") return std::string("segment '") + std::string(seg) + "' is a dot segment, which requests never have";
      if (seg.find('%') != std::string_view::npos)
        return std::string("segment '") + std::string(seg) + "' has a '%'; write the character itself, not its percent-encoding";
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

  Headers() = default;  // no storage until the first field
  Headers(const Headers& o) { copy_from(o); }
  Headers(Headers&& o) noexcept : items_(std::move(o.items_)), n_(o.n_) {
    o.items_.clear();
    o.n_ = 0;
  }
  Headers& operator=(const Headers& o) {
    if (this != &o) copy_from(o);
    return *this;
  }
  Headers& operator=(Headers&& o) noexcept {
    if (this != &o) {
      release();
      items_ = std::move(o.items_);
      n_ = o.n_;
      o.items_.clear();
      o.n_ = 0;
    }
    return *this;
  }
  ~Headers() { release(); }

  void add(std::string_view name, std::string_view value) {
    auto& kv = next();
    kv.first.assign(name);
    for (auto& c : kv.first) c = ascii_lower(c);
    kv.second.assign(value);
  }
  /// Replaces every field named `name` with one, or adds it.
  void set(std::string_view name, std::string_view value) {
    auto e = items_.begin() + std::ptrdiff_t(n_);
    auto it = std::find_if(items_.begin(), e, [&](auto& kv) { return iequals(kv.first, name); });
    if (it == e) return add(name, value);
    it->second.assign(value);
    shrink_to(std::remove_if(it + 1, e, [&](auto& kv) { return iequals(kv.first, name); }));
  }
  void remove(std::string_view name) {
    auto e = items_.begin() + std::ptrdiff_t(n_);
    shrink_to(std::remove_if(items_.begin(), e, [&](auto& kv) { return iequals(kv.first, name); }));
  }
  [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const {
    for (auto& [k, v] : *this)
      if (iequals(k, name)) return std::string_view(v);
    return std::nullopt;
  }
  [[nodiscard]] bool contains(std::string_view name) const { return get(name).has_value(); }
  using const_iterator = std::vector<value_type>::const_iterator;
  [[nodiscard]] const_iterator begin() const { return items_.cbegin(); }
  [[nodiscard]] const_iterator end() const { return items_.cbegin() + std::ptrdiff_t(n_); }
  [[nodiscard]] std::size_t size() const { return n_; }
  [[nodiscard]] bool empty() const { return n_ == 0; }

  static bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
  }

 private:
  static constexpr char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

  // Every request and response builds a few header lists. Their storage (the
  // vector and the strings in it, with their capacity) goes back to a small
  // per-thread pool when a list is destroyed, and the next list on that thread
  // reuses it: in steady state adding a field allocates nothing. Fields past
  // n_ are spares whose strings keep their capacity.
  static constexpr std::size_t kPoolLists = 64;      // lists kept per thread
  static constexpr std::size_t kPoolMaxFields = 32;  // larger lists are freed
  static constexpr std::size_t kPoolMaxBytes = 512;  // as are lists holding a longer string
  struct Pool {
    std::vector<std::vector<value_type>> lists;
    /// This thread's pool; null once the thread is exiting.
    static Pool* get() {
      thread_local bool gone = false;  // trivially destructible: readable at any point of thread exit
      if (gone) return nullptr;
      thread_local struct Holder {
        Pool pool;
        ~Holder() { gone = true; }
      } holder;
      return &holder.pool;
    }
  };
  value_type& next() {
    if (n_ < items_.size()) return items_[n_++];
    if (items_.capacity() == 0) acquire();
    if (n_ < items_.size()) return items_[n_++];
    items_.emplace_back();
    return items_[n_++];
  }
  void acquire() {
    if (auto* p = Pool::get(); p && !p->lists.empty()) {
      items_ = std::move(p->lists.back());
      p->lists.pop_back();
    } else {
      items_.reserve(8);  // most messages carry a handful of fields
    }
  }
  void release() noexcept {
    if (items_.capacity() == 0) return;
    n_ = 0;
    Pool* p = Pool::get();
    if (!p || p->lists.size() >= kPoolLists || items_.capacity() > kPoolMaxFields) return;
    for (auto& [k, v] : items_) {
      if (k.capacity() > kPoolMaxBytes || v.capacity() > kPoolMaxBytes) return;
      k.clear();
      v.clear();
    }
    try {
      p->lists.push_back(std::move(items_));
    } catch (...) {  // the list is simply freed
    }
  }
  void copy_from(const Headers& o) {
    n_ = 0;
    for (auto& [k, v] : o) {
      auto& kv = next();
      kv.first.assign(k);
      kv.second.assign(v);
    }
  }
  void shrink_to(std::vector<value_type>::iterator new_end) { n_ = std::size_t(new_end - items_.begin()); }

  std::vector<value_type> items_;  // [0, n_) are the fields
  std::size_t n_ = 0;
};

}  // namespace crocket
