#pragma once
// JSON for plain aggregates, driven by reflection:
// `struct NewUser { std::string email; std::optional<int> age; };` needs no
// macros or registration to be used with Json<NewUser>.
//
// Reading decodes straight from the text into the struct, with no document
// tree in between, and is strict: the input must be valid UTF-8, an object
// may not repeat a key, and ReadOptions bounds depth, string length, members
// and elements. Writing always produces valid UTF-8.
//
//   C++                                JSON
//   bool                               true / false
//   integers, floating point           number (NaN and infinity encode as null)
//   std::string (also string_view to encode)   string
//   an enum                            string: the enumerator's name
//   an aggregate struct                object, one member per data member
//   std::optional<T>                   T or null; absent is fine when reading
//   std::vector<T>, std::array<T, N>   array (std::array: exactly N elements)
//   std::map<std::string, T>           object
//   std::variant<T...>                 the first alternative that fits (see json::tag)
//   std::monostate                     null
//   std::chrono::sys_time<D>           RFC 3339 "2026-10-05T14:03:00.250Z", or a date
//                                      "2026-10-05" when D is days
//   std::chrono::year_month_day        "2026-10-05"
//   std::chrono::duration<R, P>        number of ticks (P units)
//   json::Value                        any JSON
//
// Annotations adjust the mapping per member, per struct or per enum:
//
//   struct [[= json::rename_all(json::camel_case), = json::deny_unknown_fields]] Signup {
//     std::string display_name;                                 // "displayName"
//     [[= json::rename("e-mail"), = json::email]] std::string email;
//     [[= json::min_len(8), = json::max_len(64)]] std::string password;
//     [[= json::min(13)]] std::optional<int> age;
//     [[= json::as_string]] std::uint64_t referrer_id = 0;      // "1234567890123456789"
//     [[= json::omit_null]] std::optional<std::string> note;    // left out when empty
//   };

#include "crocket/detail/regex.hpp"
#include "crocket/request.hpp"

#include <meta>
#include <array>
#include <bitset>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
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

// ---- a document, for when the shape is not known ----------------------------

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

// ---- errors ------------------------------------------------------------------

enum class Errc : std::uint8_t {
  syntax,         // not JSON (including invalid UTF-8)
  type,           // JSON, but not the shape T needs (wrong type, missing member, ...)
  unknown_field,  // a member T does not have, under deny_unknown_fields
  duplicate_key,  // an object names the same key twice
  limit,          // ReadOptions exceeded
  validation,     // a validation annotation failed; see `errors`
};

struct Error {
  Errc code = Errc::syntax;
  std::string message;                // what is wrong, without the location
  std::string pointer;                // RFC 6901 pointer to the value; "" is the whole document
  std::size_t line = 1, column = 1;   // where reading stopped
  std::vector<FieldError> errors;     // validation: every failing field

  /// One sentence for a client: "malformed JSON at line 1, column 9: ..." or
  /// "field '/email': expected string, got number".
  std::string describe() const;
};

// ---- annotations ---------------------------------------------------------------

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

/// The JSON name of a member, of an enumerator, or of a struct used as a
/// json::tag alternative: `[[= json::rename("userId")]] std::uint64_t user_id;`.
struct Rename {
  const char* name;
  constexpr bool operator==(const Rename&) const = default;
};
consteval Rename rename(std::string_view name) {
  if (name.empty()) throw std::meta::exception(u8"crocket: json::rename(\"\") needs a name", ^^Rename);
  return Rename{std::define_static_string(name)};
}

/// Naming convention for every member of a struct (or enumerator of an enum)
/// without its own json::rename. Identifiers may be snake_case or camelCase.
enum class Case : std::uint8_t { snake, camel, pascal, kebab, screaming_snake };
inline constexpr Case snake_case = Case::snake;                    // display_name
inline constexpr Case camel_case = Case::camel;                    // displayName
inline constexpr Case pascal_case = Case::pascal;                  // DisplayName
inline constexpr Case kebab_case = Case::kebab;                    // display-name
inline constexpr Case screaming_snake_case = Case::screaming_snake;  // DISPLAY_NAME

struct RenameAll {
  Case style;
  constexpr bool operator==(const RenameAll&) const = default;
};
consteval RenameAll rename_all(Case style) { return RenameAll{style}; }

/// On a struct: a member the struct does not have is 422 json.unknown_field
/// instead of being ignored.
struct DenyUnknownFields {
  constexpr bool operator==(const DenyUnknownFields&) const = default;
};
inline constexpr DenyUnknownFields deny_unknown_fields{};

/// On a std::optional member, or a struct (all its optional members): an empty
/// optional is left out of the output instead of written as null.
struct OmitNull {
  constexpr bool operator==(const OmitNull&) const = default;
};
inline constexpr OmitNull omit_null{};

/// On an integer member, or a struct (all its integer members): written as a
/// JSON string, "9007199254740993", so JavaScript clients, whose numbers lose
/// precision above 2^53, see the exact value. Reading accepts a string or a number.
struct AsString {
  constexpr bool operator==(const AsString&) const = default;
};
inline constexpr AsString as_string{};

/// On a std::chrono::sys_time member: a Unix timestamp (an integer) instead of
/// an RFC 3339 string.
struct UnixTime {
  bool millis;
  constexpr bool operator==(const UnixTime&) const = default;
};
inline constexpr UnixTime unix_seconds{false};
inline constexpr UnixTime unix_millis{true};

/// On a std::variant member whose alternatives are structs: an internally
/// tagged union. The object carries its alternative's name in member `field`:
///
///   struct Circle { double r; };
///   struct [[= json::rename("rect")]] Rectangle { double w, h; };
///   struct Shape { [[= json::tag("kind")]] std::variant<Circle, Rectangle> geometry; };
///   // {"geometry": {"kind": "Circle", "r": 1}}  {"geometry": {"kind": "rect", "w": 2, "h": 3}}
///
/// Without json::tag a variant is untagged: reading picks the first alternative
/// whose JSON type fits and that reads without error.
struct Tag {
  const char* field;
  constexpr bool operator==(const Tag&) const = default;
};
consteval Tag tag(std::string_view field) {
  if (field.empty()) throw std::meta::exception(u8"crocket: json::tag(\"\") needs a member name", ^^Tag);
  return Tag{std::define_static_string(field)};
}

// Validation. A failing check does not stop reading: every failure is reported,
// as 422 json.validation with one entry per field in "errors". Checks on a
// std::optional member apply when it holds a value.

/// Numbers: lower and upper bounds, inclusive.
struct Min {
  bool integral;
  long long i;
  double d;
  constexpr bool operator==(const Min&) const = default;
};
struct Max {
  bool integral;
  long long i;
  double d;
  constexpr bool operator==(const Max&) const = default;
};
consteval Min min(std::integral auto v) { return Min{true, static_cast<long long>(v), 0}; }
consteval Min min(std::floating_point auto v) { return Min{false, 0, static_cast<double>(v)}; }
consteval Max max(std::integral auto v) { return Max{true, static_cast<long long>(v), 0}; }
consteval Max max(std::floating_point auto v) { return Max{false, 0, static_cast<double>(v)}; }

/// Strings (in code points) and arrays and maps (in elements): length bounds.
struct MinLen {
  std::size_t n;
  constexpr bool operator==(const MinLen&) const = default;
};
struct MaxLen {
  std::size_t n;
  constexpr bool operator==(const MaxLen&) const = default;
};
consteval MinLen min_len(std::size_t n) { return MinLen{n}; }
consteval MaxLen max_len(std::size_t n) { return MaxLen{n}; }

/// Strings: must match the regular expression somewhere (anchor with ^...$).
/// See detail/regex.hpp for the syntax; an invalid pattern is a compile error.
struct Pattern {
  const char* source;
  crocket::detail::regex::Program program;
  constexpr bool operator==(const Pattern& o) const { return source == o.source; }
};
consteval Pattern pattern(std::string_view re) {
  auto c = crocket::detail::regex::Compiler(re).run();
  auto code = std::define_static_array(c.code);
  auto ranges = std::define_static_array(c.ranges);
  return Pattern{std::define_static_string(re),
                 {code.data(), static_cast<std::uint32_t>(code.size()), ranges.data()}};
}

/// Strings: an email address (local@domain.tld, RFC 5321 lengths).
struct Email {
  constexpr bool operator==(const Email&) const = default;
};
inline constexpr Email email{};

// ---- reading -----------------------------------------------------------------

namespace detail {

/// Where a value sits in the document: a linked list on the decoder's stack,
/// turned into a pointer only when something fails.
struct Path {
  const Path* parent = nullptr;
  std::string_view key = {};
  std::size_t index = 0;
  bool is_index = false;
};
std::string pointer(const Path* p);  // RFC 6901

/// Remembers keys to reject a repeated one; allocates only once used.
class KeySet {
 public:
  bool insert(std::string_view k) {
    if (big_.empty()) {
      for (auto& s : small_)
        if (s == k) return false;
      if (small_.size() < 16) {
        small_.emplace_back(k);
        return true;
      }
      big_.insert(std::make_move_iterator(small_.begin()), std::make_move_iterator(small_.end()));
      small_.clear();
    }
    return big_.emplace(k).second;
  }

 private:
  std::vector<std::string> small_;
  std::unordered_set<std::string> big_;
};

bool valid_email(std::string_view s);
std::size_t code_points(std::string_view s);
void write_date(std::string& out, std::int64_t days);
void write_datetime(std::string& out, std::int64_t unix_seconds, std::uint64_t fraction, int digits);
/// RFC 3339 date-time with offset, as UTC seconds and nanoseconds.
bool parse_datetime(std::string_view s, std::int64_t& unix_seconds, std::uint32_t& nanos);
/// "YYYY-MM-DD" as days since 1970-01-01.
bool parse_date(std::string_view s, std::int64_t& days);

}  // namespace detail

/// A cursor over JSON text, for decoders. Every method that can fail returns
/// false after recording the first error; decoders return false up the stack.
class Reader {
 public:
  using Path = detail::Path;

  explicit Reader(std::string_view text, const ReadOptions& opts = {})
      : begin_(text.data()), p_(text.data()), end_(text.data() + text.size()), opts_(opts) {}

  const ReadOptions& options() const { return opts_; }

  void ws() {
    while (p_ != end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
  }
  /// The next byte (after ws()), or '\0' at the end.
  char peek() const { return p_ != end_ ? *p_ : '\0'; }
  bool at_end() const { return p_ == end_; }

  bool null(const Path* path);
  bool boolean(bool& out, const Path* path);
  /// A string's contents. Points into the input when there are no escapes,
  /// otherwise into a buffer that the next string() call reuses.
  bool string(std::string_view& out, const Path* path);
  /// A number's text, checked against the JSON grammar.
  bool number(std::string_view& text, bool& integral, const Path* path);
  /// Any value, fully checked (syntax, UTF-8, duplicates, limits) and dropped.
  bool skip(const Path* path);

  bool begin_object(const Path* path);
  /// The next member's key, positioned at its value: 1. End of object: 0. Error: -1.
  int next_member(std::string_view& key, std::size_t& count, const Path* path);
  bool begin_array(const Path* path);
  /// Positioned at the next element: 1. End of array: 0. Error: -1.
  int next_element(std::size_t& count, const Path* path);

  /// Records an error at the current position; returns false.
  bool fail(Errc code, const Path* path, std::string message);
  /// "expected <want>, got <what is there>"; a syntax error if nothing valid is there.
  bool mismatch(const Path* path, std::string_view want);
  void violation(const Path* path, std::string detail) {
    violations_.push_back({detail::pointer(path), std::move(detail)});
  }

  bool failed() const { return failed_; }
  Errc error_code() const { return error_.code; }
  Error take_error() { return std::move(error_); }
  std::vector<FieldError>& violations() { return violations_; }

  /// For trying alternatives: save, try, and restore on failure.
  struct Mark {
    const char* p;
    std::size_t depth;
    std::size_t violations;
  };
  Mark mark() const { return {p_, depth_, violations_.size()}; }
  void restore(const Mark& m) {
    p_ = m.p;
    depth_ = m.depth;
    violations_.resize(m.violations);
    failed_ = false;
    error_ = {};
  }
  /// restore() after a failed attempt, charging the bytes it read to a budget
  /// of 8x the input. Nested alternatives could otherwise re-read the same
  /// subtree 2^depth times. False (a limit error) once the budget is spent.
  bool backtrack(const Mark& m, const Path* path);

 private:
  bool enter(const Path* path);
  bool literal(std::string_view lit, const Path* path);

  const char* begin_;
  const char* p_;
  const char* end_;
  std::size_t depth_ = 0;
  ReadOptions opts_;
  bool failed_ = false;
  Error error_;
  std::vector<FieldError> violations_;
  std::string scratch_;
  std::size_t backtracked_ = 0;
};

// ---- type traits ---------------------------------------------------------------

template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_vector : std::false_type {};
template <class T, class A> struct is_vector<std::vector<T, A>> : std::true_type {};
template <class T> struct is_std_array : std::false_type {};
template <class T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <class T> struct is_string_map : std::false_type {};
template <class V, class C, class A> struct is_string_map<std::map<std::string, V, C, A>> : std::true_type {};
template <class T> struct is_variant : std::false_type {};
template <class... T> struct is_variant<std::variant<T...>> : std::true_type {};
template <class T> struct is_sys_time : std::false_type {};
template <class D> struct is_sys_time<std::chrono::time_point<std::chrono::system_clock, D>> : std::true_type {};
template <class T> struct is_duration : std::false_type {};
template <class R, class P> struct is_duration<std::chrono::duration<R, P>> : std::true_type {};

/// A plain aggregate struct whose public data members are encoded by name.
template <class T>
concept ReflectableStruct = std::is_class_v<T> && std::is_aggregate_v<T> && !is_std_array<T>::value &&
                            !std::is_same_v<T, std::monostate>;

template <class T>
constexpr bool encodable() {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, bool> || std::is_arithmetic_v<U> || std::is_enum_v<U>) return true;
  else if constexpr (std::is_convertible_v<const U&, std::string_view>) return true;
  else if constexpr (std::is_same_v<U, Value> || std::is_same_v<U, std::monostate>) return true;
  else if constexpr (is_sys_time<U>::value || is_duration<U>::value ||
                     std::is_same_v<U, std::chrono::year_month_day>) return true;
  else if constexpr (is_optional<U>::value) return encodable<typename U::value_type>();
  else if constexpr (is_vector<U>::value || is_std_array<U>::value) return encodable<typename U::value_type>();
  else if constexpr (is_string_map<U>::value) return encodable<typename U::mapped_type>();
  else if constexpr (is_variant<U>::value)
    return []<class... A>(std::type_identity<std::variant<A...>>) { return (encodable<A>() && ...); }(
        std::type_identity<U>{});
  else if constexpr (ReflectableStruct<U>) return true;  // members checked when instantiated
  else return false;
}
template <class T>
concept Encodable = encodable<T>();

namespace detail {

// ---- compile-time names and checks -------------------------------------------

consteval std::string convert_case(std::string_view id, Case style) {
  std::vector<std::string> words;
  std::string cur;
  auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
  auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  for (std::size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    if (c == '_' || c == '-') {
      if (!cur.empty()) words.push_back(cur), cur.clear();
      continue;
    }
    // A word starts at an upper-case letter after a lower-case letter or digit
    // ("userId"), or before a lower-case one in a run of capitals ("HTTPServer").
    if (upper(c) && !cur.empty() &&
        (lower(cur.back()) || digit(cur.back()) || (i + 1 < id.size() && lower(id[i + 1]) && upper(cur.back()))))
      words.push_back(cur), cur.clear();
    cur += c;
  }
  if (!cur.empty()) words.push_back(cur);
  auto to_lower = [&](std::string w) {
    for (auto& c : w) if (upper(c)) c = char(c - 'A' + 'a');
    return w;
  };
  auto to_upper = [&](std::string w) {
    for (auto& c : w) if (lower(c)) c = char(c - 'a' + 'A');
    return w;
  };
  auto capital = [&](std::string w) {
    w = to_lower(w);
    if (!w.empty() && lower(w[0])) w[0] = char(w[0] - 'a' + 'A');
    return w;
  };
  std::string out;
  for (std::size_t i = 0; i < words.size(); ++i) {
    switch (style) {
      case Case::snake: out += (i ? "_" : "") + to_lower(words[i]); break;
      case Case::kebab: out += (i ? "-" : "") + to_lower(words[i]); break;
      case Case::screaming_snake: out += (i ? "_" : "") + to_upper(words[i]); break;
      case Case::camel: out += i ? capital(words[i]) : to_lower(words[i]); break;
      case Case::pascal: out += capital(words[i]); break;
    }
  }
  return out;
}

template <class A>
consteval bool has(std::meta::info r) {
  return !std::meta::annotations_of_with_type(r, ^^A).empty();
}
template <class A>
consteval A get(std::meta::info r) {
  return std::meta::extract<A>(std::meta::annotations_of_with_type(r, ^^A)[0]);
}

/// The JSON name of a data member or an enumerator.
consteval std::string json_name(std::meta::info r) {
  if (has<Rename>(r)) return get<Rename>(r).name;
  auto parent = std::meta::parent_of(r);
  if (has<RenameAll>(parent)) return convert_case(std::meta::identifier_of(r), get<RenameAll>(parent).style);
  return std::string(std::meta::identifier_of(r));
}

/// The tag value naming struct `t` in a json::tag variant.
consteval std::string tag_name(std::meta::info t) {
  t = std::meta::dealias(t);
  if (has<Rename>(t)) return get<Rename>(t).name;
  if (!std::meta::has_identifier(t))
    throw std::meta::exception(u8"crocket: a json::tag alternative without a simple name needs [[= json::rename(...)]]",
                               ^^Tag);
  return std::string(std::meta::identifier_of(t));
}

/// `"text"` as a JSON string literal.
consteval std::string quoted(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      constexpr char hex[] = "0123456789abcdef";
      out += "\\u00";
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    } else out += c;
  }
  return out + "\"";
}

consteval std::vector<std::meta::info> members_of(std::meta::info type) {
  return std::meta::nonstatic_data_members_of(type, std::meta::access_context::current());
}

consteval std::vector<const char*> member_names(std::meta::info type) {
  std::vector<const char*> out;
  for (auto m : members_of(type)) out.push_back(std::define_static_string(json_name(m)));
  return out;
}

consteval std::vector<const char*> member_keys(std::meta::info type) {  // "\"name\":"
  std::vector<const char*> out;
  for (auto m : members_of(type)) out.push_back(std::define_static_string(quoted(json_name(m)) + ":"));
  return out;
}

consteval std::vector<const char*> enumerator_names(std::meta::info type) {
  std::vector<const char*> out;
  for (auto e : std::meta::enumerators_of(type)) out.push_back(std::define_static_string(json_name(e)));
  return out;
}

/// Empty when two members do not share a JSON name, otherwise a diagnostic.
consteval std::string name_clash(std::meta::info type) {
  auto ms = members_of(type);
  for (std::size_t i = 0; i < ms.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (json_name(ms[i]) == json_name(ms[j]))
        return "crocket: members '" + std::string(std::meta::identifier_of(ms[j])) + "' and '" +
               std::string(std::meta::identifier_of(ms[i])) + "' of " + std::string(std::meta::display_string_of(type)) +
               " both have the JSON name \"" + json_name(ms[i]) + "\"";
  return {};
}

consteval std::string enum_clash(std::meta::info type) {
  auto es = std::meta::enumerators_of(type);
  if (es.empty()) return "crocket: enum " + std::string(std::meta::display_string_of(type)) + " has no enumerators to name in JSON";
  for (std::size_t i = 0; i < es.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (json_name(es[i]) == json_name(es[j]))
        return "crocket: enumerators '" + std::string(std::meta::identifier_of(es[j])) + "' and '" +
               std::string(std::meta::identifier_of(es[i])) + "' of " + std::string(std::meta::display_string_of(type)) +
               " both have the JSON name \"" + json_name(es[i]) + "\"";
  return {};
}

/// How a member's annotations change the way its value (and the values inside
/// its optional, vector, array or map) is written and read.
enum class TimeFormat : std::uint8_t { rfc3339, unix_seconds, unix_millis };
struct FieldOpts {
  bool as_string = false;
  TimeFormat time = TimeFormat::rfc3339;
  const char* tag = nullptr;
  constexpr bool operator==(const FieldOpts&) const = default;
};

consteval FieldOpts field_opts(std::meta::info member) {
  FieldOpts o;
  o.as_string = has<AsString>(member) || has<AsString>(std::meta::parent_of(member));
  if (has<UnixTime>(member)) o.time = get<UnixTime>(member).millis ? TimeFormat::unix_millis : TimeFormat::unix_seconds;
  if (has<Tag>(member)) o.tag = get<Tag>(member).field;
  return o;
}

template <class T> struct unwrap { using type = T; };
template <class T> struct unwrap<std::optional<T>> : unwrap<T> {};
template <class T, class A> struct unwrap<std::vector<T, A>> : unwrap<T> {};
template <class T, std::size_t N> struct unwrap<std::array<T, N>> : unwrap<T> {};
template <class V, class C, class A> struct unwrap<std::map<std::string, V, C, A>> : unwrap<V> {};
/// The element type under optionals and containers: vector<optional<int>> -> int.
template <class T> using unwrap_t = typename unwrap<T>::type;

template <class T> using unopt_t = std::conditional_t<is_optional<T>::value, typename unwrap<T>::type, T>;

template <class T> inline constexpr bool is_number_v = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;
template <class T> inline constexpr bool is_sized_v = std::is_same_v<T, std::string> || is_vector<T>::value ||
                                                      is_std_array<T>::value || is_string_map<T>::value;

template <class V>
inline constexpr bool tag_alternatives_ok = [] {
  if constexpr (is_optional<V>::value) return tag_alternatives_ok<typename V::value_type>;
  else if constexpr (is_variant<V>::value)
    return []<class... A>(std::type_identity<std::variant<A...>>) { return (ReflectableStruct<A> && ...); }(
        std::type_identity<V>{});
  else return false;
}();

/// Compile-time checks for struct T: names, and that each annotation fits its member.
template <class T>
consteval bool check_struct() {
  constexpr std::string_view clash = std::define_static_string(name_clash(^^T));
  static_assert(clash.empty(), clash);
  template for (constexpr auto m : std::define_static_array(members_of(^^T))) {
    using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
    using E = unopt_t<M>;
    constexpr auto where = [](std::string_view ann, std::string_view needs) consteval {
      return std::define_static_string("crocket: [[= json::" + std::string(ann) + "]] on member '" +
                                       std::string(std::meta::identifier_of(m)) + "' of " +
                                       std::string(std::meta::display_string_of(^^T)) + " needs " +
                                       std::string(needs) + ", but the member is " +
                                       std::string(std::meta::display_string_of(std::meta::type_of(m))));
    };
    if constexpr (has<Min>(m) || has<Max>(m))
      static_assert(is_number_v<E>, std::string_view(where("min/max", "a number")));
    if constexpr (has<MinLen>(m) || has<MaxLen>(m))
      static_assert(is_sized_v<E>, std::string_view(where("min_len/max_len", "a string, array or map")));
    if constexpr (has<Pattern>(m))
      static_assert(std::is_same_v<E, std::string>, std::string_view(where("pattern", "a std::string")));
    if constexpr (has<Email>(m))
      static_assert(std::is_same_v<E, std::string>, std::string_view(where("email", "a std::string")));
    if constexpr (has<AsString>(m))
      static_assert(std::is_integral_v<unwrap_t<M>> && !std::is_same_v<unwrap_t<M>, bool>,
                    std::string_view(where("as_string", "an integer (or an optional or container of one)")));
    if constexpr (has<UnixTime>(m))
      static_assert(is_sys_time<unwrap_t<M>>::value,
                    std::string_view(where("unix_seconds/unix_millis", "a std::chrono::sys_time")));
    if constexpr (has<Tag>(m))
      static_assert(tag_alternatives_ok<M>, std::string_view(where("tag", "a std::variant of structs")));
    if constexpr (has<OmitNull>(m))
      static_assert(is_optional<M>::value, std::string_view(where("omit_null", "a std::optional")));
  }
  return true;
}

template <class T>
consteval bool check_enum() {
  constexpr std::string_view clash = std::define_static_string(enum_clash(^^T));
  static_assert(clash.empty(), clash);
  return true;
}

// ---- validation ------------------------------------------------------------------

void bound_message(std::string& out, std::string_view word, bool integral, long long i, double d);

template <class V>
std::size_t length_of(const V& v) {
  if constexpr (std::is_same_v<V, std::string>) return code_points(v);
  else return v.size();
}

template <class V>
bool below(const V& v, bool integral, long long i, double d) {
  if constexpr (std::is_integral_v<V>) return integral ? std::cmp_less(v, i) : double(v) < d;
  else return v < (integral ? static_cast<V>(i) : static_cast<V>(d));
}
template <class V>
bool above(const V& v, bool integral, long long i, double d) {
  if constexpr (std::is_integral_v<V>) return integral ? std::cmp_greater(v, i) : double(v) > d;
  else return v > (integral ? static_cast<V>(i) : static_cast<V>(d));
}

consteval bool has_checks(std::meta::info m) {
  return has<Min>(m) || has<Max>(m) || has<MinLen>(m) || has<MaxLen>(m) || has<Pattern>(m) || has<Email>(m);
}

/// Runs member `Mem`'s validation annotations on `v`; failures go to `out`.
template <std::meta::info Mem, class V>
void check_member(const V& v, const Path* path, std::vector<FieldError>& out) {
  if constexpr (is_optional<V>::value) {
    if (v) check_member<Mem>(*v, path, out);
  } else {
    auto fail = [&](std::string detail) { out.push_back({pointer(path), std::move(detail)}); };
    if constexpr (has<Min>(Mem)) {
      constexpr Min b = get<Min>(Mem);
      if (below(v, b.integral, b.i, b.d)) {
        std::string s;
        bound_message(s, "at least", b.integral, b.i, b.d);
        fail(std::move(s));
      }
    }
    if constexpr (has<Max>(Mem)) {
      constexpr Max b = get<Max>(Mem);
      if (above(v, b.integral, b.i, b.d)) {
        std::string s;
        bound_message(s, "at most", b.integral, b.i, b.d);
        fail(std::move(s));
      }
    }
    auto count = [](std::size_t n) {
      constexpr bool text = std::is_same_v<V, std::string>;
      return std::to_string(n) + (text ? " character" : " element") + (n == 1 ? "" : "s");
    };
    if constexpr (has<MinLen>(Mem)) {
      constexpr std::size_t n = get<MinLen>(Mem).n;
      if (length_of(v) < n) fail("must have at least " + count(n));
    }
    if constexpr (has<MaxLen>(Mem)) {
      constexpr std::size_t n = get<MaxLen>(Mem).n;
      if (length_of(v) > n) fail("must have at most " + count(n));
    }
    if constexpr (has<Pattern>(Mem)) {
      constexpr Pattern p = get<Pattern>(Mem);
      if (!crocket::detail::regex::search(p.program, v)) fail("must match the pattern " + std::string(p.source));
    }
    if constexpr (has<Email>(Mem)) {
      if (!valid_email(v)) fail("must be an email address");
    }
  }
}

template <class T>
void validate_into(const T& v, const Path* path, std::vector<FieldError>& out);

// ---- variant helpers -------------------------------------------------------------

enum : unsigned { k_null = 1, k_bool = 2, k_number = 4, k_string = 8, k_array = 16, k_object = 32, k_any = 63 };

/// The JSON types that a T (with options O) can be read from.
template <class T, FieldOpts O>
consteval unsigned kinds() {
  if constexpr (std::is_same_v<T, bool>) return k_bool;
  else if constexpr (std::is_integral_v<T>) return O.as_string ? (k_number | k_string) : k_number;
  else if constexpr (std::is_floating_point_v<T> || is_duration<T>::value) return k_number;
  else if constexpr (std::is_same_v<T, std::string> || std::is_enum_v<T> ||
                     std::is_same_v<T, std::chrono::year_month_day>) return k_string;
  else if constexpr (is_sys_time<T>::value) return O.time == TimeFormat::rfc3339 ? k_string : k_number;
  else if constexpr (std::is_same_v<T, std::monostate>) return k_null;
  else if constexpr (std::is_same_v<T, Value>) return k_any;
  else if constexpr (is_optional<T>::value) return k_null | kinds<typename T::value_type, O>();
  else if constexpr (is_vector<T>::value || is_std_array<T>::value) return k_array;
  else if constexpr (is_variant<T>::value)
    return O.tag ? k_object : []<class... A>(std::type_identity<std::variant<A...>>) {
      return (kinds<A, O>() | ...);
    }(std::type_identity<T>{});
  else return k_object;  // structs and maps
}

consteval std::string kinds_text(unsigned k) {
  std::string out;
  std::string_view names[] = {"null", "boolean", "number", "string", "array", "object"};
  std::vector<std::string_view> parts;
  for (int i = 0; i < 6; ++i)
    if (k & (1u << i)) parts.push_back(names[i]);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += i + 1 == parts.size() ? " or " : ", ";
    out += parts[i];
  }
  return out;
}

inline unsigned kind_at(char c) {
  switch (c) {
    case 'n': return k_null;
    case 't': case 'f': return k_bool;
    case '"': return k_string;
    case '[': return k_array;
    case '{': return k_object;
    default: return (c == '-' || (c >= '0' && c <= '9')) ? unsigned(k_number) : 0u;
  }
}

// ---- the decoder -----------------------------------------------------------------

bool read_value(Reader& r, Value& out, const Path* path);

template <class T, FieldOpts O = FieldOpts{}>
bool read(Reader& r, T& out, const Path* path);

template <class T>
bool read_struct(Reader& r, T& out, const Path* path, std::string_view tag_field = {});

template <class I>
bool parse_integer(Reader& r, std::string_view text, I& out, const Path* path) {
  if constexpr (std::is_unsigned_v<I>) {
    if (text.size() > 1 && text[0] == '-') {  // only "-0" fits
      if (text.find_first_not_of('0', 1) == std::string_view::npos) { out = 0; return true; }
      return r.fail(Errc::type, path, "integer out of range");
    }
  }
  auto res = std::from_chars(text.data(), text.data() + text.size(), out);
  if (res.ec == std::errc::result_out_of_range) return r.fail(Errc::type, path, "integer out of range");
  if (res.ec != std::errc() || res.ptr != text.data() + text.size())
    return r.fail(Errc::type, path, "expected integer, got \"" + std::string(text) + "\"");
  return true;
}

template <class I, FieldOpts O>
bool read_integer(Reader& r, I& out, const Path* path) {
  std::string_view text;
  if (O.as_string && r.peek() == '"') {
    if (!r.string(text, path)) return false;
    return parse_integer(r, text, out, path);
  }
  if (detail::kind_at(r.peek()) != k_number) return r.mismatch(path, O.as_string ? "integer or string" : "integer");
  bool integral;
  if (!r.number(text, integral, path)) return false;
  if (!integral) return r.fail(Errc::type, path, "expected integer, got " + std::string(text));
  return parse_integer(r, text, out, path);
}

template <class F>
bool read_float(Reader& r, F& out, const Path* path) {
  if (detail::kind_at(r.peek()) != k_number) return r.mismatch(path, "number");
  std::string_view text;
  bool integral;
  if (!r.number(text, integral, path)) return false;
  auto res = std::from_chars(text.data(), text.data() + text.size(), out);
  if (res.ec != std::errc()) return r.fail(Errc::type, path, "number out of range");
  return true;
}

template <class E>
bool read_enum(Reader& r, E& out, const Path* path) {
  static_assert(check_enum<E>());
  if (r.peek() != '"') return r.mismatch(path, "string");
  std::string_view s;
  if (!r.string(s, path)) return false;
  static constexpr auto names = std::define_static_array(enumerator_names(^^E));
  bool found = false;
  std::size_t i = 0;
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (!found && s == std::string_view(names[i])) {
      out = [:e:];
      found = true;
    }
    ++i;
  }
  if (found) return true;
  static constexpr std::string_view allowed = std::define_static_string([] consteval {
    std::string out;
    for (auto n : enumerator_names(^^E)) out += std::string(out.empty() ? "" : ", ") + quoted(n);
    return out;
  }());
  return r.fail(Errc::type, path, "expected one of " + std::string(allowed));
}

template <class Tp, FieldOpts O>
bool read_time(Reader& r, Tp& out, const Path* path) {
  using D = typename Tp::duration;
  using namespace std::chrono;
  if constexpr (O.time != TimeFormat::rfc3339) {
    std::int64_t n;
    if (!read_integer<std::int64_t, FieldOpts{}>(r, n, path)) return false;
    if constexpr (O.time == TimeFormat::unix_millis) out = floor<D>(sys_time<milliseconds>(milliseconds(n)));
    else out = floor<D>(sys_seconds(seconds(n)));
    return true;
  } else {
    if (r.peek() != '"') return r.mismatch(path, "string");
    std::string_view s;
    if (!r.string(s, path)) return false;
    if constexpr (std::ratio_greater_equal_v<typename D::period, days::period>) {
      std::int64_t d;
      if (!parse_date(s, d)) return r.fail(Errc::type, path, "expected a date (YYYY-MM-DD)");
      out = floor<D>(sys_days(days(d)));
    } else {
      std::int64_t secs;
      std::uint32_t nanos;
      if (!parse_datetime(s, secs, nanos))
        return r.fail(Errc::type, path, "expected an RFC 3339 timestamp (2026-10-05T14:03:00Z)");
      if constexpr (!treat_as_floating_point_v<typename D::rep>) {
        constexpr auto lo = duration_cast<seconds>(D::min()).count(), hi = duration_cast<seconds>(D::max()).count();
        if (secs <= lo || secs >= hi) return r.fail(Errc::type, path, "timestamp out of range");
      }
      out = floor<D>(sys_seconds(seconds(secs))) + floor<D>(nanoseconds(nanos));
    }
    return true;
  }
}

template <class T, FieldOpts O>
bool read_variant(Reader& r, T& out, const Path* path) {
  constexpr std::size_t N = std::variant_size_v<T>;
  if constexpr (O.tag != nullptr) {
    constexpr std::string_view field = O.tag;
    static constexpr auto tags = std::define_static_array([] consteval {
      std::vector<const char*> v;
      template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, std::variant_size_v<T>)))
        v.push_back(std::define_static_string(tag_name(^^std::variant_alternative_t<i, T>)));
      return v;
    }());
    // Find the tag wherever it is in the object, then read the whole object again
    // as that alternative.
    auto start = r.mark();
    if (r.peek() != '{') return r.mismatch(path, "object");
    if (!r.begin_object(path)) return false;
    std::string name;
    bool found = false;
    std::string_view key;
    std::size_t n = 0;
    while (true) {
      int k = r.next_member(key, n, path);
      if (k < 0) return false;
      if (k == 0) break;
      Path child{path, field};
      if (key == field) {
        if (found) return r.fail(Errc::duplicate_key, path, "duplicate key \"" + std::string(field) + "\"");
        if (r.peek() != '"') return r.mismatch(&child, "string");
        std::string_view v;
        if (!r.string(v, &child)) return false;
        name.assign(v);
        found = true;
      } else {
        std::string other_key(key);  // skip() reuses the buffer `key` may point into
        Path other{path, other_key};
        if (!r.skip(&other)) return false;
      }
    }
    if (!found) return r.fail(Errc::type, path, "missing member \"" + std::string(field) + "\"");
    r.restore(start);
    bool ok = false, matched = false;
    template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, N))) {
      if (!matched && name == std::string_view(tags[i])) {
        matched = true;
        ok = read_struct(r, out.template emplace<i>(), path, field);
      }
    }
    if (matched) return ok;
    static constexpr std::string_view allowed = std::define_static_string([] consteval {
      std::string s;
      for (auto t : tags) s += std::string(s.empty() ? "" : ", ") + quoted(t);
      return s;
    }());
    Path child{path, field};
    return r.fail(Errc::type, &child, "unknown \"" + std::string(field) + "\" \"" + name + "\"; expected one of " +
                                          std::string(allowed));
  } else {
    constexpr unsigned all = kinds<T, O>();
    unsigned k = kind_at(r.peek());
    if (!(k & all)) {
      static constexpr std::string_view want = std::define_static_string(kinds_text(all));
      return r.mismatch(path, want);
    }
    std::size_t candidates = 0;
    template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, N)))
      if (kinds<std::variant_alternative_t<i, T>, O>() & k) ++candidates;
    // One alternative fits this JSON type: its errors are the useful ones.
    // Several: try them in order and keep the first that reads.
    bool done = false, ok = false;
    auto start = r.mark();
    template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, N))) {
      using A = std::variant_alternative_t<i, T>;
      if (!done && (kinds<A, O>() & k)) {
        if (candidates == 1) {
          done = true;
          ok = read<A, O>(r, out.template emplace<i>(), path);
        } else if (read<A, O>(r, out.template emplace<i>(), path)) {
          done = ok = true;
        } else if (r.error_code() != Errc::type && r.error_code() != Errc::unknown_field) {
          done = true;  // the document itself is bad (syntax, limits, duplicates): no alternative fits
        } else if (!r.backtrack(start, path)) {
          done = true;
        }
      }
    }
    if (done) return ok;
    static constexpr std::string_view type = std::define_static_string(std::meta::display_string_of(^^T));
    return r.fail(Errc::type, path, "value matches no alternative of " + std::string(type));
  }
}

template <class T, FieldOpts O>
bool read(Reader& r, T& out, const Path* path) {
  using U = T;
  if constexpr (std::is_same_v<U, bool>) {
    return r.boolean(out, path);
  } else if constexpr (std::is_integral_v<U>) {
    return read_integer<U, O>(r, out, path);
  } else if constexpr (std::is_floating_point_v<U>) {
    return read_float(r, out, path);
  } else if constexpr (std::is_enum_v<U>) {
    return read_enum(r, out, path);
  } else if constexpr (std::is_same_v<U, std::string>) {
    if (r.peek() != '"') return r.mismatch(path, "string");
    std::string_view s;
    if (!r.string(s, path)) return false;
    out.assign(s);
    return true;
  } else if constexpr (std::is_same_v<U, Value>) {
    return read_value(r, out, path);
  } else if constexpr (std::is_same_v<U, std::monostate>) {
    return r.null(path);
  } else if constexpr (is_sys_time<U>::value) {
    return read_time<U, O>(r, out, path);
  } else if constexpr (std::is_same_v<U, std::chrono::year_month_day>) {
    if (r.peek() != '"') return r.mismatch(path, "string");
    std::string_view s;
    std::int64_t d;
    if (!r.string(s, path)) return false;
    if (!parse_date(s, d)) return r.fail(Errc::type, path, "expected a date (YYYY-MM-DD)");
    out = std::chrono::year_month_day(std::chrono::sys_days(std::chrono::days(d)));
    return true;
  } else if constexpr (is_duration<U>::value) {
    typename U::rep n{};
    if (!read<typename U::rep, FieldOpts{}>(r, n, path)) return false;
    out = U(n);
    return true;
  } else if constexpr (is_optional<U>::value) {
    if (r.peek() == 'n') {
      out.reset();
      return r.null(path);
    }
    return read<typename U::value_type, O>(r, out.emplace(), path);
  } else if constexpr (is_vector<U>::value || is_std_array<U>::value) {
    if (r.peek() != '[') return r.mismatch(path, "array");
    if (!r.begin_array(path)) return false;
    if constexpr (is_vector<U>::value) out.clear();
    std::size_t n = 0;
    while (true) {
      int k = r.next_element(n, path);
      if (k < 0) return false;
      if (k == 0) break;
      Path child{path, {}, n - 1, true};
      if constexpr (is_vector<U>::value) {
        if (!read<typename U::value_type, O>(r, out.emplace_back(), &child)) return false;
      } else {
        if (n > out.size()) return r.fail(Errc::type, path, "expected an array of " + std::to_string(out.size()) + " elements");
        if (!read<typename U::value_type, O>(r, out[n - 1], &child)) return false;
      }
    }
    if constexpr (is_std_array<U>::value)
      if (n != out.size()) return r.fail(Errc::type, path, "expected an array of " + std::to_string(out.size()) + " elements");
    return true;
  } else if constexpr (is_string_map<U>::value) {
    if (r.peek() != '{') return r.mismatch(path, "object");
    if (!r.begin_object(path)) return false;
    out.clear();
    std::string_view key;
    std::size_t n = 0;
    while (true) {
      int k = r.next_member(key, n, path);
      if (k < 0) return false;
      if (k == 0) break;
      auto [it, fresh] = out.try_emplace(std::string(key));
      if (!fresh) return r.fail(Errc::duplicate_key, path, "duplicate key \"" + it->first + "\"");
      Path child{path, it->first};
      if (!read<typename U::mapped_type, O>(r, it->second, &child)) return false;
    }
    return true;
  } else if constexpr (is_variant<U>::value) {
    return read_variant<U, O>(r, out, path);
  } else if constexpr (ReflectableStruct<U>) {
    return read_struct(r, out, path);
  } else {
    static_assert(false, "crocket::json: type is not JSON-decodable");
  }
}

template <class T>
bool read_struct(Reader& r, T& out, const Path* path, std::string_view tag_field) {
  static_assert(check_struct<T>());
  static constexpr auto members = std::define_static_array(members_of(^^T));
  constexpr std::size_t N = members.size();
  static constexpr auto name_ptrs = std::define_static_array(member_names(^^T));
  static constexpr auto names = [] {
    std::array<std::string_view, N> a{};
    for (std::size_t i = 0; i < N; ++i) a[i] = name_ptrs[i];
    return a;
  }();
  constexpr bool deny_unknown = has<DenyUnknownFields>(^^T);

  if (r.peek() != '{') return r.mismatch(path, "object");
  if (!r.begin_object(path)) return false;
  std::bitset<N> seen;
  KeySet others;  // unknown keys, to reject a repeated one
  std::string_view key;
  std::size_t n = 0, hint = 0;
  while (true) {
    int k = r.next_member(key, n, path);
    if (k < 0) return false;
    if (k == 0) break;
    // Members usually arrive in declaration order: try the one after the last match first.
    std::size_t idx = N;
    if (hint < N && names[hint] == key) idx = hint;
    else
      for (std::size_t i = 0; i < N; ++i)
        if (names[i] == key) { idx = i; break; }
    if (idx == N) {
      std::string unknown(key);  // skip() reuses the buffer `key` may point into
      Path child{path, unknown};
      if (!tag_field.empty() && key == tag_field) {
        if (!r.skip(&child)) return false;
        continue;
      }
      if (deny_unknown || r.options().deny_unknown_fields)
        return r.fail(Errc::unknown_field, &child, "unknown field \"" + std::string(key) + "\"");
      if (!others.insert(key)) return r.fail(Errc::duplicate_key, path, "duplicate key \"" + std::string(key) + "\"");
      if (!r.skip(&child)) return false;
      continue;
    }
    if (seen[idx]) return r.fail(Errc::duplicate_key, path, "duplicate key \"" + std::string(key) + "\"");
    seen[idx] = true;
    hint = idx + 1;
    Path child{path, names[idx]};
    bool ok = true;
    template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, N))) {
      if (idx == i) {
        constexpr auto m = members[i];
        using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
        ok = read<M, field_opts(m)>(r, out.[:m:], &child);
        if constexpr (has_checks(m))
          if (ok) check_member<m>(out.[:m:], &child, r.violations());
      }
    }
    if (!ok) return false;
  }
  // Absent members are fine if they are optional or have a default member initializer.
  template for (constexpr auto i : std::define_static_array(std::views::iota(std::size_t{0}, N))) {
    constexpr auto m = members[i];
    using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
    if constexpr (!is_optional<M>::value && !std::meta::has_default_member_initializer(m)) {
      if (!seen[i]) {
        Path child{path, names[i]};
        return r.fail(Errc::type, &child, "required field is missing");
      }
    }
  }
  return true;
}

template <class T>
void validate_into(const T& v, const Path* path, std::vector<FieldError>& out) {
  if constexpr (is_optional<T>::value) {
    if (v) validate_into(*v, path, out);
  } else if constexpr (is_vector<T>::value || is_std_array<T>::value) {
    for (std::size_t i = 0; i < v.size(); ++i) {
      Path child{path, {}, i, true};
      validate_into(v[i], &child, out);
    }
  } else if constexpr (is_string_map<T>::value) {
    for (auto& [k, e] : v) {
      Path child{path, k};
      validate_into(e, &child, out);
    }
  } else if constexpr (is_variant<T>::value) {
    std::visit([&](const auto& alt) { validate_into(alt, path, out); }, v);
  } else if constexpr (ReflectableStruct<T>) {
    static_assert(check_struct<T>());
    static constexpr auto names = std::define_static_array(member_names(^^T));
    std::size_t i = 0;
    template for (constexpr auto m : std::define_static_array(members_of(^^T))) {
      Path child{path, names[i++]};
      if constexpr (has_checks(m)) check_member<m>(v.[:m:], &child, out);
      validate_into(v.[:m:], &child, out);
    }
  }
}

// ---- the encoder -----------------------------------------------------------------

template <class T, FieldOpts O = FieldOpts{}>
void write(std::string& out, const T& v);

template <class T>
void write_struct(std::string& out, const T& v, std::string_view prefix = {});

}  // namespace detail

void write_string(std::string& out, std::string_view s);  // quoted, escaped, invalid UTF-8 replaced with U+FFFD
void dump(const Value& v, std::string& out);

namespace detail {

template <class I>
void write_integer(std::string& out, I v, bool quoted) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  if (quoted) out += '"';
  out.append(buf, r.ptr);
  if (quoted) out += '"';
}

template <class Tp, FieldOpts O>
void write_time(std::string& out, const Tp& tp) {
  using D = typename Tp::duration;
  using namespace std::chrono;
  if constexpr (O.time == TimeFormat::unix_seconds) {
    write_integer(out, floor<seconds>(tp).time_since_epoch().count(), false);
  } else if constexpr (O.time == TimeFormat::unix_millis) {
    write_integer(out, floor<milliseconds>(tp).time_since_epoch().count(), false);
  } else if constexpr (std::ratio_greater_equal_v<typename D::period, days::period>) {
    out += '"';
    write_date(out, floor<days>(tp).time_since_epoch().count());
    out += '"';
  } else {
    using P = typename D::period;
    // Fraction digits: none for whole seconds, k for 10^-k, nanoseconds otherwise.
    constexpr int digits = [] {
      if constexpr (std::ratio_greater_equal_v<P, std::ratio<1>>) return 0;
      else {
        if (P::num != 1) return 9;
        std::intmax_t den = P::den;
        int k = 0;
        while (den % 10 == 0) den /= 10, ++k;
        return den == 1 && k <= 9 ? k : 9;
      }
    }();
    auto s = floor<seconds>(tp);
    std::uint64_t frac = 0;
    if constexpr (digits > 0) {
      constexpr std::intmax_t scale = [] {
        std::intmax_t v = 1;
        for (int i = 0; i < digits; ++i) v *= 10;
        return v;
      }();
      frac = static_cast<std::uint64_t>(floor<duration<std::int64_t, std::ratio<1, scale>>>(tp - s).count());
    }
    out += '"';
    write_datetime(out, s.time_since_epoch().count(), frac, digits);
    out += '"';
  }
}

template <class E>
void write_enum(std::string& out, E v) {
  static_assert(check_enum<E>());
  static constexpr auto names = std::define_static_array([] consteval {
    std::vector<const char*> out;
    for (auto n : enumerator_names(^^E)) out.push_back(std::define_static_string(quoted(n)));
    return out;
  }());
  std::size_t i = 0;
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (v == [:e:]) {
      out += names[i];
      return;
    }
    ++i;
  }
  // Not a named value (a combination of flags, say): the number.
  write_integer(out, static_cast<std::underlying_type_t<E>>(v), false);
}

template <class T, FieldOpts O>
void write(std::string& out, const T& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    out += v ? "true" : "false";
  } else if constexpr (std::is_integral_v<U>) {
    write_integer(out, v, O.as_string);
  } else if constexpr (std::is_floating_point_v<U>) {
    if (!std::isfinite(v)) { out += "null"; return; }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
  } else if constexpr (std::is_enum_v<U>) {
    write_enum(out, v);
  } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
    write_string(out, std::string_view(v));
  } else if constexpr (std::is_same_v<U, Value>) {
    dump(v, out);
  } else if constexpr (std::is_same_v<U, std::monostate>) {
    out += "null";
  } else if constexpr (is_sys_time<U>::value) {
    write_time<U, O>(out, v);
  } else if constexpr (std::is_same_v<U, std::chrono::year_month_day>) {
    out += '"';
    write_date(out, std::chrono::sys_days(v).time_since_epoch().count());
    out += '"';
  } else if constexpr (is_duration<U>::value) {
    write<typename U::rep, FieldOpts{}>(out, v.count());
  } else if constexpr (is_optional<U>::value) {
    if (v) write<typename U::value_type, O>(out, *v);
    else out += "null";
  } else if constexpr (is_vector<U>::value || is_std_array<U>::value) {
    out += '[';
    bool first = true;
    for (auto& e : v) {
      if (!first) out += ',';
      first = false;
      write<typename U::value_type, O>(out, e);
    }
    out += ']';
  } else if constexpr (is_string_map<U>::value) {
    out += '{';
    bool first = true;
    for (auto& [k, e] : v) {
      if (!first) out += ',';
      first = false;
      write_string(out, k);
      out += ':';
      write<typename U::mapped_type, O>(out, e);
    }
    out += '}';
  } else if constexpr (is_variant<U>::value) {
    if constexpr (O.tag != nullptr) {
      std::visit(
          [&]<class A>(const A& alt) {
            static constexpr std::string_view prefix =
                std::define_static_string(quoted(O.tag) + ":" + quoted(tag_name(^^A)));
            write_struct(out, alt, prefix);
          },
          v);
    } else {
      std::visit([&]<class A>(const A& alt) { write<A, O>(out, alt); }, v);
    }
  } else if constexpr (ReflectableStruct<U>) {
    write_struct(out, v);
  } else {
    static_assert(false, "crocket::json: type is not JSON-encodable");
  }
}

template <class T>
void write_struct(std::string& out, const T& v, std::string_view prefix) {
  static_assert(check_struct<T>());
  static constexpr auto keys = std::define_static_array(member_keys(^^T));
  constexpr bool omit_all = has<OmitNull>(^^T);
  out += '{';
  out += prefix;
  bool first = prefix.empty();
  std::size_t i = 0;
  template for (constexpr auto m : std::define_static_array(members_of(^^T))) {
    using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
    const auto& field = v.[:m:];
    bool skip = false;
    if constexpr (is_optional<M>::value && (omit_all || has<OmitNull>(m))) skip = !field.has_value();
    if (!skip) {
      if (!first) out += ',';
      first = false;
      out += keys[i];
      write<M, field_opts(m)>(out, field);
    }
    ++i;
  }
  out += '}';
}

}  // namespace detail

// ---- public API ------------------------------------------------------------------

/// Parses any JSON into a document.
std::expected<Value, Error> parse(std::string_view text, const ReadOptions& opts = {});

/// Reads `text` into `out`, member by member, with no intermediate document.
/// Validation annotations are checked along the way: if any fail, the error is
/// Errc::validation and lists every failure. On error `out` is partly written.
template <class T>
std::expected<void, Error> read(std::string_view text, T& out, const ReadOptions& opts = {}) {
  Reader r(text, opts);
  r.ws();
  if (detail::read<T>(r, out, nullptr)) {
    r.ws();
    if (!r.at_end()) r.fail(Errc::syntax, nullptr, "unexpected trailing characters");
  } else if (!r.failed()) {
    r.fail(Errc::syntax, nullptr, "invalid JSON");
  }
  if (r.failed()) return std::unexpected(r.take_error());
  if (!r.violations().empty()) {
    Error e{Errc::validation, "", "", 0, 0, std::move(r.violations())};
    return std::unexpected(std::move(e));
  }
  return {};
}

template <class T>
std::expected<T, Error> from_string(std::string_view text, const ReadOptions& opts = {}) {
  T out{};
  if (auto ok = read(text, out, opts); !ok) return std::unexpected(std::move(ok.error()));
  return out;
}

/// Decodes a parsed document into `out` (by writing it out and reading it back).
template <class T>
std::expected<void, Error> decode(const Value& v, T& out) {
  std::string text;
  dump(v, text);
  return read(text, out);
}

/// Checks the validation annotations of `v` and everything inside it; empty if all pass.
template <class T>
std::vector<FieldError> validate(const T& v) {
  std::vector<FieldError> out;
  detail::validate_into(v, nullptr, out);
  return out;
}

template <class T>
void encode(std::string& out, const T& v) {
  detail::write<std::remove_cvref_t<T>>(out, v);
}

template <class T>
std::string to_string(const T& v) {
  std::string s;
  encode(s, v);
  return s;
}

}  // namespace json
}  // namespace crocket
