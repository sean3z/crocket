#pragma once
// Protocol Buffers (proto3) for plain aggregates, driven by reflection: no
// .proto file to keep in sync and no generated code. A struct's data members
// are its fields.
//
//   struct HelloRequest {
//     std::string name;                          // field 1
//     std::int32_t age = 0;                      // field 2
//     [[= proto::field(5)]] std::string email;   // field 5
//     std::vector<std::string> tags;             // field 6
//   };
//
// Field numbers follow declaration order from 1. [[= proto::field(n)]] pins a
// member to n and numbering continues from n + 1. Renumbering a field breaks
// clients built against the old schema, so pin numbers before reordering or
// removing members.
//
//   C++                          proto3
//   bool                         bool
//   int8/16/32_t, int64_t        int32, int64
//   uint8/16/32_t, uint64_t      uint32, uint64
//   float, double                float, double
//   std::string                  string (UTF-8 is checked when decoding)
//   proto::Bytes                 bytes
//   an enum                      enum
//   an aggregate struct          message
//   std::optional<T>             optional T (explicit presence)
//   std::vector<T>               repeated T (numbers packed)
//   std::map<K, V>               map<K, V>
//
// Decoding skips unknown fields, so old servers accept newer clients' messages.

#include "crocket/json.hpp"

#include <meta>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crocket::proto {

/// Field number annotation: `[[= proto::field(3)]] std::string email;`.
struct Field {
  int number;
  constexpr bool operator==(const Field&) const = default;
};
consteval Field field(int number) { return Field{number}; }

/// The C++ type for proto `bytes`.
using Bytes = std::vector<std::byte>;

/// A struct that maps to a protobuf message.
template <class T>
concept Message = std::is_class_v<T> && std::is_aggregate_v<T> && !json::is_std_array<T>::value;

namespace detail {

template <class T> struct is_map : std::false_type {};
template <class K, class V, class C, class A> struct is_map<std::map<K, V, C, A>> : std::true_type {};
template <class K, class V, class H, class E, class A>
struct is_map<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template <class T>
inline constexpr bool is_repeated_v = json::is_vector<T>::value && !std::is_same_v<T, Bytes>;

enum class Kind : std::uint8_t { None, Bool, Int32, Int64, UInt32, UInt64, Float, Double, String, Bytes, Enum, Message };
enum class Wire : std::uint8_t { Varint = 0, I64 = 1, Len = 2, I32 = 5 };

template <class T>
consteval Kind kind_of() {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>) return Kind::Bool;
  else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, wchar_t> || std::is_same_v<U, char8_t> ||
                     std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>)
    return Kind::None;  // text, not numbers: use std::string
  else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
    return sizeof(U) <= 4 ? Kind::Int32 : sizeof(U) == 8 ? Kind::Int64 : Kind::None;
  else if constexpr (std::is_integral_v<U>)
    return sizeof(U) <= 4 ? Kind::UInt32 : sizeof(U) == 8 ? Kind::UInt64 : Kind::None;
  else if constexpr (std::is_same_v<U, float>) return Kind::Float;
  else if constexpr (std::is_same_v<U, double>) return Kind::Double;
  else if constexpr (std::is_same_v<U, std::string>) return Kind::String;
  else if constexpr (std::is_same_v<U, Bytes>) return Kind::Bytes;
  else if constexpr (std::is_enum_v<U>) return sizeof(U) <= 4 ? Kind::Enum : Kind::None;
  else if constexpr (Message<U>) return Kind::Message;
  else return Kind::None;
}

template <class T>
consteval Wire wire_of() {
  constexpr Kind k = kind_of<T>();
  if constexpr (k == Kind::Float) return Wire::I32;
  else if constexpr (k == Kind::Double) return Wire::I64;
  else if constexpr (k == Kind::String || k == Kind::Bytes || k == Kind::Message) return Wire::Len;
  else return Wire::Varint;
}

/// Numbers (varint and fixed width) are packed in repeated fields.
template <class T>
inline constexpr bool packable_v = wire_of<T>() != Wire::Len;

template <class K>
consteval bool map_key_ok() {
  constexpr Kind k = kind_of<K>();
  return k == Kind::Bool || k == Kind::Int32 || k == Kind::Int64 || k == Kind::UInt32 || k == Kind::UInt64 ||
         k == Kind::String;
}

/// Whether a data member's type has a protobuf mapping.
template <class M>
consteval bool field_ok() {
  if constexpr (json::is_optional<M>::value) return kind_of<typename M::value_type>() != Kind::None;
  else if constexpr (is_repeated_v<M>) return kind_of<typename M::value_type>() != Kind::None;
  else if constexpr (is_map<M>::value)
    return map_key_ok<typename M::key_type>() && kind_of<typename M::mapped_type>() != Kind::None;
  else return kind_of<M>() != Kind::None;
}

// ---- field numbers ------------------------------------------------------------

inline constexpr int kMaxFieldNumber = (1 << 29) - 1;

consteval std::vector<std::meta::info> fields_of(std::meta::info type) {
  return std::meta::nonstatic_data_members_of(type, std::meta::access_context::current());
}

consteval std::vector<int> field_numbers(std::meta::info type) {
  std::vector<int> out;
  int next = 1;
  for (auto m : fields_of(type)) {
    auto anns = std::meta::annotations_of_with_type(m, ^^Field);
    int n = anns.empty() ? next : std::meta::extract<Field>(anns[0]).number;
    out.push_back(n);
    next = n + 1;
  }
  return out;
}

consteval int field_number(std::meta::info member) {
  auto ms = fields_of(std::meta::parent_of(member));
  auto nums = field_numbers(std::meta::parent_of(member));
  for (std::size_t i = 0; i < ms.size(); ++i)
    if (ms[i] == member) return nums[i];
  return 0;
}

/// Empty when T's field numbers are valid, otherwise a diagnostic.
consteval std::string numbering_problem(std::meta::info type) {
  auto ms = fields_of(type);
  auto nums = field_numbers(type);
  std::string where = "crocket: proto message " + std::string(std::meta::display_string_of(type));
  for (std::size_t i = 0; i < ms.size(); ++i) {
    std::string name(std::meta::identifier_of(ms[i]));
    if (std::meta::annotations_of_with_type(ms[i], ^^Field).size() > 1)
      return where + ": member '" + name + "' has more than one proto::field annotation";
    int n = nums[i];
    if (n < 1 || n > kMaxFieldNumber)
      return where + ": member '" + name + "' has field number " + crocket::detail::decimal(n) +
             ", outside 1..536870911";
    if (n >= 19000 && n <= 19999)
      return where + ": member '" + name + "' has field number " + crocket::detail::decimal(n) +
             ", which protobuf reserves (19000..19999)";
    for (std::size_t j = 0; j < i; ++j)
      if (nums[j] == n)
        return where + ": members '" + std::string(std::meta::identifier_of(ms[j])) + "' and '" + name +
               "' both have field number " + crocket::detail::decimal(n) + "; pin one with [[= proto::field(n)]]";
  }
  return {};
}

/// Compile-time checks for message T: numbering and member types.
template <class T>
constexpr bool validate() {
  constexpr std::string_view problem = std::define_static_string(numbering_problem(^^T));
  static_assert(problem.empty(), problem);
  template for (constexpr auto m : std::define_static_array(fields_of(^^T))) {
    using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
    constexpr std::string_view name = std::meta::identifier_of(m);
    static_assert(field_ok<M>(),
                  std::string("crocket: member '") + std::string(name) + "' of proto message " +
                      std::string(std::meta::display_string_of(^^T)) + " has type " +
                      std::string(std::meta::display_string_of(std::meta::type_of(m))) +
                      ", which has no protobuf mapping (use bool, intN_t, uintN_t, float, double, std::string, "
                      "proto::Bytes, an enum, a message struct, or std::optional, std::vector or std::map of "
                      "those)");
  }
  return true;
}

// ---- encoding -------------------------------------------------------------------

inline void put_varint(std::string& o, std::uint64_t v) {
  while (v >= 0x80) {
    o += static_cast<char>((v & 0x7f) | 0x80);
    v >>= 7;
  }
  o += static_cast<char>(v);
}
inline void put_tag(std::string& o, int number, Wire w) {
  put_varint(o, (std::uint64_t(number) << 3) | std::uint64_t(w));
}
template <class U>
void put_fixed(std::string& o, U v) {
  for (std::size_t i = 0; i < sizeof(U); ++i) o += static_cast<char>((v >> (8 * i)) & 0xff);
}

template <class T>
void encode_message(std::string& o, const T& v);

/// The value alone, without its tag.
template <class T>
void encode_value(std::string& o, const T& v) {
  constexpr Kind k = kind_of<T>();
  if constexpr (k == Kind::Bool) put_varint(o, v ? 1 : 0);
  else if constexpr (k == Kind::Int32 || k == Kind::Int64) put_varint(o, std::uint64_t(std::int64_t(v)));
  else if constexpr (k == Kind::UInt32 || k == Kind::UInt64) put_varint(o, std::uint64_t(v));
  else if constexpr (k == Kind::Enum)
    put_varint(o, std::uint64_t(std::int64_t(static_cast<std::int32_t>(std::to_underlying(v)))));
  else if constexpr (k == Kind::Float) put_fixed(o, std::bit_cast<std::uint32_t>(v));
  else if constexpr (k == Kind::Double) put_fixed(o, std::bit_cast<std::uint64_t>(v));
  else if constexpr (k == Kind::String) {
    put_varint(o, v.size());
    o += v;
  } else if constexpr (k == Kind::Bytes) {
    put_varint(o, v.size());
    o.append(reinterpret_cast<const char*>(v.data()), v.size());
  } else if constexpr (k == Kind::Message) {
    std::string sub;
    encode_message(sub, v);
    put_varint(o, sub.size());
    o += sub;
  } else {
    static_assert(false, "crocket::proto: type has no protobuf mapping");
  }
}

/// proto3 implicit presence: scalars at their default value are not sent.
template <class T>
bool is_default(const T& v) {
  constexpr Kind k = kind_of<T>();
  if constexpr (k == Kind::Float) return std::bit_cast<std::uint32_t>(v) == 0;
  else if constexpr (k == Kind::Double) return std::bit_cast<std::uint64_t>(v) == 0;
  else if constexpr (k == Kind::String || k == Kind::Bytes) return v.empty();
  else if constexpr (k == Kind::Enum) return std::to_underlying(v) == 0;
  else if constexpr (k == Kind::Message) return false;  // messages have presence: always sent
  else return v == T{};
}

template <class M>
void encode_field(std::string& o, int number, const M& v) {
  if constexpr (json::is_optional<M>::value) {
    if (!v) return;
    put_tag(o, number, wire_of<typename M::value_type>());
    encode_value(o, *v);
  } else if constexpr (is_repeated_v<M>) {
    using X = typename M::value_type;
    if constexpr (packable_v<X>) {
      if (v.empty()) return;
      std::string packed;
      for (const X& e : v) encode_value<X>(packed, e);
      put_tag(o, number, Wire::Len);
      put_varint(o, packed.size());
      o += packed;
    } else {
      for (const X& e : v) {
        put_tag(o, number, wire_of<X>());
        encode_value<X>(o, e);
      }
    }
  } else if constexpr (is_map<M>::value) {
    using K = typename M::key_type;
    using V = typename M::mapped_type;
    for (const auto& [key, value] : v) {
      std::string entry;
      put_tag(entry, 1, wire_of<K>());
      encode_value<K>(entry, key);
      put_tag(entry, 2, wire_of<V>());
      encode_value<V>(entry, value);
      put_tag(o, number, Wire::Len);
      put_varint(o, entry.size());
      o += entry;
    }
  } else {
    if (is_default(v)) return;
    put_tag(o, number, wire_of<M>());
    encode_value(o, v);
  }
}

template <class T>
void encode_message(std::string& o, const T& v) {
  static_assert(validate<T>());
  template for (constexpr auto m : std::define_static_array(fields_of(^^T))) {
    constexpr int number = field_number(m);
    encode_field(o, number, v.[:m:]);
  }
}

// ---- decoding --------------------------------------------------------------------

struct Reader {
  const unsigned char* p;
  const unsigned char* end;
  [[nodiscard]] bool empty() const { return p == end; }
  [[nodiscard]] std::size_t size() const { return std::size_t(end - p); }
};

/// A decode failure. `path` is the dotted field path, filled in on the way out.
struct Error {
  std::string path;
  std::string what;
};
using Status = std::expected<void, Error>;

inline std::unexpected<Error> fail(std::string what) { return std::unexpected(Error{{}, std::move(what)}); }

inline constexpr int kMaxDepth = 100;

inline bool read_varint(Reader& r, std::uint64_t& v) {
  v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (r.empty()) return false;
    std::uint8_t b = *r.p++;
    v |= std::uint64_t(b & 0x7f) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;  // more than 10 bytes
}
template <class U>
bool read_fixed(Reader& r, U& v) {
  if (r.size() < sizeof(U)) return false;
  v = 0;
  for (std::size_t i = 0; i < sizeof(U); ++i) v |= U(r.p[i]) << (8 * i);
  r.p += sizeof(U);
  return true;
}
inline Status read_len(Reader& r, Reader& sub) {
  std::uint64_t n;
  if (!read_varint(r, n)) return fail("truncated length");
  if (n > r.size()) return fail("length " + std::to_string(n) + " runs past the end of the message");
  sub = Reader{r.p, r.p + n};
  r.p += n;
  return {};
}

inline bool valid_utf8(std::string_view s) {
  auto* p = reinterpret_cast<const unsigned char*>(s.data());
  auto* end = p + s.size();
  while (p < end) {
    unsigned c = *p++;
    if (c < 0x80) continue;
    int extra;
    unsigned cp, min;
    if ((c & 0xe0) == 0xc0) extra = 1, cp = c & 0x1f, min = 0x80;
    else if ((c & 0xf0) == 0xe0) extra = 2, cp = c & 0x0f, min = 0x800;
    else if ((c & 0xf8) == 0xf0) extra = 3, cp = c & 0x07, min = 0x10000;
    else return false;
    if (end - p < extra) return false;
    for (int i = 0; i < extra; ++i) {
      if ((p[i] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (p[i] & 0x3f);
    }
    p += extra;
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;  // overlong, too big, surrogate
  }
  return true;
}

/// Skips a field this message does not know.
inline Status skip(Wire w, Reader& r) {
  std::uint64_t u;
  Reader sub;
  switch (static_cast<int>(w)) {
    case 0: return read_varint(r, u) ? Status{} : fail("truncated varint");
    case 1: return read_fixed(r, u) ? Status{} : fail("truncated fixed64");
    case 2: return read_len(r, sub);
    case 5: {
      std::uint32_t x;
      return read_fixed(r, x) ? Status{} : fail("truncated fixed32");
    }
    case 3:
    case 4: return fail("groups are not supported");
    default: return fail("invalid wire type " + std::to_string(int(w)));
  }
}

consteval std::string_view kind_name(Kind k) {
  switch (k) {
    case Kind::Bool: return "bool";
    case Kind::Int32: return "int32";
    case Kind::Int64: return "int64";
    case Kind::UInt32: return "uint32";
    case Kind::UInt64: return "uint64";
    case Kind::Float: return "float";
    case Kind::Double: return "double";
    case Kind::String: return "string";
    case Kind::Bytes: return "bytes";
    case Kind::Enum: return "enum";
    case Kind::Message: return "message";
    default: return "?";
  }
}

template <class T>
Status decode_message(Reader r, T& out, int depth);

template <class T>
Status decode_value(Wire w, Reader& r, T& v, int depth) {
  constexpr Kind k = kind_of<T>();
  if (w != wire_of<T>())
    return fail("expected " + std::string(kind_name(k)) + ", got wire type " + std::to_string(int(w)));
  if constexpr (wire_of<T>() == Wire::Varint) {
    std::uint64_t u;
    if (!read_varint(r, u)) return fail("truncated varint");
    if constexpr (k == Kind::Bool) {
      v = u != 0;
    } else if constexpr (k == Kind::Int32 || k == Kind::UInt32 || k == Kind::Enum) {
      // 32-bit fields keep the low 32 bits; narrower C++ types must hold the value.
      using Target = typename std::conditional_t<k == Kind::Enum, std::underlying_type<T>, std::type_identity<T>>::type;
      using Wide = std::conditional_t<k == Kind::UInt32, std::uint32_t, std::int32_t>;
      auto x = static_cast<Wide>(static_cast<std::uint32_t>(u));
      if (!std::in_range<Target>(x)) return fail("value " + std::to_string(x) + " out of range");
      v = static_cast<T>(static_cast<Target>(x));
    } else {
      v = static_cast<T>(u);
    }
  } else if constexpr (k == Kind::Float) {
    std::uint32_t x;
    if (!read_fixed(r, x)) return fail("truncated float");
    v = std::bit_cast<float>(x);
  } else if constexpr (k == Kind::Double) {
    std::uint64_t x;
    if (!read_fixed(r, x)) return fail("truncated double");
    v = std::bit_cast<double>(x);
  } else {
    Reader sub;
    if (auto s = read_len(r, sub); !s) return s;
    if constexpr (k == Kind::String) {
      v.assign(reinterpret_cast<const char*>(sub.p), sub.size());
      if (!valid_utf8(v)) return fail("string is not valid UTF-8");
    } else if constexpr (k == Kind::Bytes) {
      v.assign(reinterpret_cast<const std::byte*>(sub.p), reinterpret_cast<const std::byte*>(sub.end));
    } else {
      if (depth >= kMaxDepth) return fail("messages nested too deeply");
      return decode_message(sub, v, depth + 1);
    }
  }
  return {};
}

template <class M>
Status decode_field(Wire w, Reader& r, M& f, int depth) {
  if constexpr (json::is_optional<M>::value) {
    if (!f) f.emplace();  // a repeated message field merges into the one already read
    return decode_value(w, r, *f, depth);
  } else if constexpr (is_repeated_v<M>) {
    using X = typename M::value_type;
    if constexpr (packable_v<X>) {
      if (w == Wire::Len) {  // packed; unpacked is accepted too
        Reader sub;
        if (auto s = read_len(r, sub); !s) return s;
        while (!sub.empty()) {
          X x{};
          if (auto s = decode_value(wire_of<X>(), sub, x, depth); !s) return s;
          f.push_back(std::move(x));
        }
        return {};
      }
    }
    X x{};
    if (auto s = decode_value(w, r, x, depth); !s) return s;
    f.push_back(std::move(x));
    return {};
  } else if constexpr (is_map<M>::value) {
    if (w != Wire::Len) return fail("expected map entry, got wire type " + std::to_string(int(w)));
    Reader sub;
    if (auto s = read_len(r, sub); !s) return s;
    typename M::key_type key{};
    typename M::mapped_type value{};
    while (!sub.empty()) {
      std::uint64_t tag;
      if (!read_varint(sub, tag)) return fail("truncated map entry");
      auto ew = static_cast<Wire>(tag & 7);
      Status s = (tag >> 3) == 1   ? decode_value(ew, sub, key, depth)
                 : (tag >> 3) == 2 ? decode_value(ew, sub, value, depth)
                                   : skip(ew, sub);
      if (!s) return s;
    }
    f.insert_or_assign(std::move(key), std::move(value));
    return {};
  } else {
    return decode_value(w, r, f, depth);
  }
}

template <class T>
Status decode_message(Reader r, T& out, int depth) {
  static_assert(validate<T>());
  while (!r.empty()) {
    std::uint64_t tag;
    if (!read_varint(r, tag)) return fail("truncated tag");
    auto number = tag >> 3;
    auto w = static_cast<Wire>(tag & 7);
    if (number == 0 || number > std::uint64_t(kMaxFieldNumber)) return fail("invalid field number");
    bool known = false;
    Status st;
    template for (constexpr auto m : std::define_static_array(fields_of(^^T))) {
      if (number == std::uint64_t(field_number(m))) {
        known = true;
        st = decode_field(w, r, out.[:m:], depth);
        if (!st) {
          constexpr std::string_view name = std::meta::identifier_of(m);
          auto& p = st.error().path;
          p = p.empty() ? std::string(name) : std::string(name) + "." + p;
        }
      }
    }
    if (!st) return st;
    if (!known)
      if (auto s = skip(w, r); !s) return s;
  }
  return {};
}

}  // namespace detail

/// Appends the wire encoding of `v` to `out`.
template <Message T>
void encode(std::string& out, const T& v) {
  detail::encode_message(out, v);
}
template <Message T>
std::string encode(const T& v) {
  std::string out;
  detail::encode_message(out, v);
  return out;
}

/// Decodes a message. On failure returns a client-safe message naming the
/// field, e.g. "field 'address.zip': expected string, got wire type 0".
template <Message T>
std::expected<T, std::string> decode(std::string_view bytes) {
  T out{};
  auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
  if (auto s = detail::decode_message(detail::Reader{p, p + bytes.size()}, out, 0); !s) {
    auto& e = s.error();
    return std::unexpected((e.path.empty() ? std::string("message") : "field '" + e.path + "'") + ": " + e.what);
  }
  return out;
}

// ---- .proto text -----------------------------------------------------------------

namespace detail {

consteval bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
consteval bool is_lower_or_digit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

/// "user_admin" -> "UserAdmin", "sayHello" -> "SayHello".
consteval std::string pascal_case(std::string_view s) {
  std::string out;
  bool up = true;
  for (char c : s) {
    if (c == '_') {
      up = true;
      continue;
    }
    out += (up && c >= 'a' && c <= 'z') ? char(c - 32) : c;
    up = false;
  }
  return out;
}

/// "HttpVerb" -> "HTTP_VERB", "darkRed" -> "DARK_RED", "dark_red" -> "DARK_RED".
consteval std::string screaming_snake(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool boundary = i > 0 && is_upper(c) &&
                    (is_lower_or_digit(s[i - 1]) || (i + 1 < s.size() && is_upper(s[i - 1]) && !is_upper(s[i + 1]) &&
                                                     s[i + 1] != '_'));
    if (boundary && !out.empty() && out.back() != '_') out += '_';
    if (c == '_' && !out.empty() && out.back() == '_') continue;
    out += (c >= 'a' && c <= 'z') ? char(c - 32) : c;
  }
  return out;
}

/// The proto name of a C++ type: its identifier, or for a template
/// specialization its spelling with every non-identifier character as '_'.
template <class T>
consteval std::string_view type_name() {
  std::string s;
  if (std::meta::has_template_arguments(^^T)) {
    for (char c : std::meta::display_string_of(^^T))
      s += (is_lower_or_digit(c) || is_upper(c)) ? c : '_';
    while (!s.empty() && s.back() == '_') s.pop_back();
  } else {
    s = std::meta::identifier_of(^^T);
  }
  return std::define_static_string(s);
}

}  // namespace detail

/// Collects the message and enum definitions a set of types needs.
class Schema {
 public:
  /// Registers message T and everything it uses; returns T's proto name.
  template <Message T>
  std::string message() {
    constexpr std::string_view name = detail::type_name<T>();
    if (!claim(name, std::meta::display_string_of(^^T))) return std::string(name);
    std::size_t slot = blocks_.size();
    blocks_.emplace_back();
    std::string body = "message " + std::string(name) + " {\n";
    static_assert(detail::validate<T>());
    template for (constexpr auto m : std::define_static_array(detail::fields_of(^^T))) {
      using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
      body += "  " + field_type<M>() + " " + std::string(std::meta::identifier_of(m)) + " = " +
              std::to_string(detail::field_number(m)) + ";\n";
    }
    blocks_[slot] = body + "}\n";
    return std::string(name);
  }

  /// Registers enum E; returns its proto name. Values are prefixed with the
  /// enum's name (Color::red -> COLOR_RED), as proto3 enum values share one
  /// scope. A missing zero value is added as <ENUM>_UNSPECIFIED.
  template <class E>
    requires std::is_enum_v<E>
  std::string enumeration() {
    constexpr std::string_view name = detail::type_name<E>();
    if (!claim(name, std::meta::display_string_of(^^E))) return std::string(name);
    constexpr std::string_view prefix = std::define_static_string(detail::screaming_snake(name));
    std::vector<std::pair<std::string, long long>> values;
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
      constexpr std::string_view v = std::define_static_string(detail::screaming_snake(std::meta::identifier_of(e)));
      std::string full = v.starts_with(std::string(prefix) + "_") ? std::string(v) : std::string(prefix) + "_" + std::string(v);
      values.emplace_back(std::move(full), static_cast<long long>([:e:]));
    }
    auto zero = std::ranges::find_if(values, [](auto& v) { return v.second == 0; });
    if (zero == values.end()) values.insert(values.begin(), {std::string(prefix) + "_UNSPECIFIED", 0});
    else std::rotate(values.begin(), zero, zero + 1);  // proto3: the first value must be zero
    bool alias = false;
    for (std::size_t i = 0; i < values.size(); ++i)
      for (std::size_t j = 0; j < i; ++j) alias = alias || values[i].second == values[j].second;
    std::string body = "enum " + std::string(name) + " {\n";
    if (alias) body += "  option allow_alias = true;\n";
    for (auto& [n, v] : values) body += "  " + n + " = " + std::to_string(v) + ";\n";
    blocks_.push_back(body + "}\n");
    return std::string(name);
  }

  /// Every definition, separated by blank lines.
  [[nodiscard]] std::string definitions() const {
    std::string out;
    for (auto& b : blocks_) out += "\n" + b;
    return out;
  }

  /// Two different C++ types with the same proto name.
  [[nodiscard]] const std::vector<std::string>& conflicts() const { return conflicts_; }

 private:
  /// True if `name` is new; false if already defined (or taken by another type).
  bool claim(std::string_view name, std::string_view cpp) {
    auto [it, inserted] = owners_.emplace(std::string(name), std::string(cpp));
    if (!inserted && it->second != cpp)
      conflicts_.push_back("proto name '" + std::string(name) + "' is used by both " + it->second + " and " +
                           std::string(cpp));
    return inserted;
  }

  template <class X>
  std::string scalar_type() {
    constexpr auto k = detail::kind_of<X>();
    if constexpr (k == detail::Kind::Enum) return enumeration<X>();
    else if constexpr (k == detail::Kind::Message) return message<X>();
    else return std::string(detail::kind_name(k));
  }

  template <class M>
  std::string field_type() {
    if constexpr (json::is_optional<M>::value) return "optional " + scalar_type<typename M::value_type>();
    else if constexpr (detail::is_repeated_v<M>) return "repeated " + scalar_type<typename M::value_type>();
    else if constexpr (detail::is_map<M>::value)
      return "map<" + scalar_type<typename M::key_type>() + ", " + scalar_type<typename M::mapped_type>() + ">";
    else return scalar_type<M>();
  }

  std::vector<std::string> blocks_;
  std::map<std::string, std::string, std::less<>> owners_;
  std::vector<std::string> conflicts_;
};

}  // namespace crocket::proto
