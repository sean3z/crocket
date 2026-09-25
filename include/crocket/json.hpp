#pragma once
// Minimal JSON: a DOM for parsing, and reflection-driven encode/decode for
// plain aggregates. `struct NewUser { std::string email; std::optional<int> age; };`
// needs no macros or registration to be used with Json<NewUser>.

#include <meta>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace crocket {

/// Json<T>: as a parameter it parses the body (422 json.invalid on failure);
/// as a return type it serializes `value` with content-type application/json.
template <class T>
struct Json {
  T value;
  T* operator->() { return &value; }
  const T* operator->() const { return &value; }
  T& operator*() { return value; }
  const T& operator*() const { return value; }
};
template <class T>
Json(T) -> Json<T>;

namespace json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // insertion order kept

class Value {
 public:
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                               std::shared_ptr<Array>, std::shared_ptr<Object>>;
  Value() : v_(nullptr) {}
  Value(std::nullptr_t) : v_(nullptr) {}
  Value(bool b) : v_(b) {}
  Value(std::int64_t i) : v_(i) {}
  Value(double d) : v_(d) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
  Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
  bool is_bool() const { return std::holds_alternative<bool>(v_); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
  bool is_number() const { return is_int() || std::holds_alternative<double>(v_); }
  bool is_string() const { return std::holds_alternative<std::string>(v_); }
  bool is_array() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
  bool is_object() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

  bool as_bool() const { return std::get<bool>(v_); }
  std::int64_t as_int() const { return std::get<std::int64_t>(v_); }
  double as_double() const { return is_int() ? double(as_int()) : std::get<double>(v_); }
  const std::string& as_string() const { return std::get<std::string>(v_); }
  const Array& as_array() const { return *std::get<std::shared_ptr<Array>>(v_); }
  const Object& as_object() const { return *std::get<std::shared_ptr<Object>>(v_); }

  /// Object member lookup; nullptr if absent or not an object.
  const Value* find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (auto& [k, v] : as_object())
      if (k == key) return &v;
    return nullptr;
  }
  std::string_view type_name() const {
    constexpr std::string_view names[] = {"null", "boolean", "number", "number", "string", "array", "object"};
    return names[v_.index()];
  }
  const Storage& storage() const { return v_; }

 private:
  Storage v_;
};

struct ParseError {
  std::size_t line = 1, column = 1;
  std::string message;
};

struct ParseLimits {
  std::size_t max_depth = 64;
};

std::expected<Value, ParseError> parse(std::string_view text, ParseLimits limits = {});
void dump(const Value& v, std::string& out);
void write_string(std::string& out, std::string_view s);  // quoted + escaped

// ---- member annotations -------------------------------------------------------

/// Marks a member as owned by the URL, so one struct serves create and update.
///
///   struct User { [[= json::from_path]] std::optional<std::uint64_t> id; std::string email; };
///
/// Enforced by the Json<T> extractor on the top-level struct T. The member must be
/// std::optional<P> where P can be parsed from a path capture (FromParam<P>).
///   - Route has a capture with the member's name ("PUT /users/{id}"): the member is
///     filled from the URL. A body value that disagrees is 422 json.path_mismatch.
///   - Route has no such capture ("POST /users"): the member is server-assigned.
///     A non-null body value is 422 json.read_only.
struct FromPath {
  constexpr bool operator==(const FromPath&) const = default;
};
inline constexpr FromPath from_path{};

// ---- reflection-driven encode ---------------------------------------------

template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_vector : std::false_type {};
template <class T, class A> struct is_vector<std::vector<T, A>> : std::true_type {};
template <class T> struct is_std_array : std::false_type {};
template <class T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <class T> struct is_string_map : std::false_type {};
template <class V, class C, class A> struct is_string_map<std::map<std::string, V, C, A>> : std::true_type {};

/// A plain aggregate struct whose public data members are encoded by name.
template <class T>
concept ReflectableStruct = std::is_class_v<T> && std::is_aggregate_v<T> && !is_std_array<T>::value;

template <class T>
constexpr bool encodable() {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, bool> || std::is_arithmetic_v<U>) return true;
  else if constexpr (std::is_convertible_v<const U&, std::string_view>) return true;
  else if constexpr (std::is_same_v<U, Value>) return true;
  else if constexpr (is_optional<U>::value) return encodable<typename U::value_type>();
  else if constexpr (is_vector<U>::value || is_std_array<U>::value) return encodable<typename U::value_type>();
  else if constexpr (is_string_map<U>::value) return encodable<typename U::mapped_type>();
  else if constexpr (ReflectableStruct<U>) return true;  // members checked when instantiated
  else return false;
}
template <class T>
concept Encodable = encodable<T>();

template <class T>
void encode(std::string& out, const T& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    out += v ? "true" : "false";
  } else if constexpr (std::is_integral_v<U>) {
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
  } else if constexpr (std::is_floating_point_v<U>) {
    if (!std::isfinite(v)) { out += "null"; return; }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
  } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
    write_string(out, std::string_view(v));
  } else if constexpr (std::is_same_v<U, Value>) {
    dump(v, out);
  } else if constexpr (is_optional<U>::value) {
    if (v) encode(out, *v); else out += "null";
  } else if constexpr (is_vector<U>::value || is_std_array<U>::value) {
    out += '[';
    bool first = true;
    for (auto& e : v) { if (!first) out += ','; first = false; encode(out, e); }
    out += ']';
  } else if constexpr (is_string_map<U>::value) {
    out += '{';
    bool first = true;
    for (auto& [k, e] : v) {
      if (!first) out += ',';
      first = false;
      write_string(out, k);
      out += ':';
      encode(out, e);
    }
    out += '}';
  } else if constexpr (ReflectableStruct<U>) {
    out += '{';
    bool first = true;
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^U, ctx))) {
      if (!first) out += ',';
      first = false;
      write_string(out, std::meta::identifier_of(m));
      out += ':';
      encode(out, v.[:m:]);
    }
    out += '}';
  } else {
    static_assert(false, "crocket::json: type is not JSON-encodable");
  }
}

template <class T>
std::string to_string(const T& v) {
  std::string s;
  encode(s, v);
  return s;
}

// ---- reflection-driven decode ---------------------------------------------

/// Decodes `v` into `out`. On failure returns a client-safe message naming the
/// field path, e.g. "field 'address.zip': expected string, got number".
template <class T>
std::expected<void, std::string> decode(const Value& v, T& out, std::string_view where = "");

namespace detail {
inline std::string at(std::string_view where) {
  return where.empty() ? std::string("body") : "field '" + std::string(where) + "'";
}
inline std::unexpected<std::string> mismatch(std::string_view where, std::string_view want, const Value& got) {
  return std::unexpected(at(where) + ": expected " + std::string(want) + ", got " + std::string(got.type_name()));
}
inline std::string join(std::string_view where, std::string_view name) {
  return where.empty() ? std::string(name) : std::string(where) + "." + std::string(name);
}
}  // namespace detail

template <class T>
std::expected<void, std::string> decode(const Value& v, T& out, std::string_view where) {
  using U = T;
  if constexpr (std::is_same_v<U, bool>) {
    if (!v.is_bool()) return detail::mismatch(where, "boolean", v);
    out = v.as_bool();
  } else if constexpr (std::is_integral_v<U>) {
    if (!v.is_int()) return detail::mismatch(where, "integer", v);
    auto i = v.as_int();
    if constexpr (std::is_unsigned_v<U>) {
      if (i < 0 || static_cast<std::uint64_t>(i) > std::numeric_limits<U>::max())
        return std::unexpected(detail::at(where) + ": integer out of range");
    } else {
      if (i < std::numeric_limits<U>::min() || i > std::numeric_limits<U>::max())
        return std::unexpected(detail::at(where) + ": integer out of range");
    }
    out = static_cast<U>(i);
  } else if constexpr (std::is_floating_point_v<U>) {
    if (!v.is_number()) return detail::mismatch(where, "number", v);
    out = static_cast<U>(v.as_double());
  } else if constexpr (std::is_same_v<U, std::string>) {
    if (!v.is_string()) return detail::mismatch(where, "string", v);
    out = v.as_string();
  } else if constexpr (std::is_same_v<U, Value>) {
    out = v;
  } else if constexpr (is_optional<U>::value) {
    if (v.is_null()) { out.reset(); return {}; }
    typename U::value_type inner{};
    if (auto r = decode(v, inner, where); !r) return r;
    out = std::move(inner);
  } else if constexpr (is_vector<U>::value) {
    if (!v.is_array()) return detail::mismatch(where, "array", v);
    out.clear();
    std::size_t i = 0;
    for (auto& e : v.as_array()) {
      typename U::value_type inner{};
      auto path = std::string(where) + "[" + std::to_string(i++) + "]";
      if (auto r = decode(e, inner, path); !r) return r;
      out.push_back(std::move(inner));
    }
  } else if constexpr (is_string_map<U>::value) {
    if (!v.is_object()) return detail::mismatch(where, "object", v);
    out.clear();
    for (auto& [k, e] : v.as_object()) {
      typename U::mapped_type inner{};
      if (auto r = decode(e, inner, detail::join(where, k)); !r) return r;
      out.insert_or_assign(k, std::move(inner));
    }
  } else if constexpr (ReflectableStruct<U>) {
    if (!v.is_object()) return detail::mismatch(where, "object", v);
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^U, ctx))) {
      using M = [:std::meta::type_of(m):];
      constexpr std::string_view name = std::meta::identifier_of(m);
      const Value* field = v.find(name);
      if (!field) {
        // Optional<T> members and members with a default initializer may be omitted.
        if constexpr (!is_optional<M>::value && !std::meta::has_default_member_initializer(m))
          return std::unexpected(detail::at(detail::join(where, name)) + ": required field is missing");
      } else if (auto r = decode(*field, out.[:m:], detail::join(where, name)); !r) {
        return r;
      }
    }
  } else {
    static_assert(false, "crocket::json: type is not JSON-decodable");
  }
  return {};
}

}  // namespace json
}  // namespace crocket
