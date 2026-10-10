#pragma once
// OpenAPI from reflection: JSON Schemas for C++ types, and the pieces of an
// operation (parameters, body, responses) for a handler's extractors and
// return type. The schemas follow json::read and json::write exactly: names
// (rename, rename_all), as_string, time formats, tagged and untagged variants,
// and the validation annotations. reflect_routes() gives every route a
// function that describes it; src/openapi.cpp assembles the document.

#include "crocket/extract.hpp"
#include "crocket/json.hpp"
#include "crocket/responder.hpp"
#include "crocket/task.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <meta>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace crocket::detail::openapi {

using json::Array;
using json::Object;
using json::Value;

/// The document's components/schemas: one per struct, named after it ("User"),
/// or after its qualified name ("billing.User") when two structs share a name.
class Components {
 public:
  /// The name for the struct `key`; `fresh` when the caller must now define it.
  std::string name(const void* key, std::string_view short_name, std::string_view qualified, bool& fresh);
  void define(const std::string& name, Value schema);
  [[nodiscard]] Object schemas() const;

 private:
  std::map<const void*, std::string> by_key_;
  std::set<std::string> taken_;
  std::vector<std::pair<std::string, Value>> defs_;  // in the order they were met
};

/// One operation (method + path) while its handler describes it.
struct Operation {
  Components& components;
  Array parameters = {};
  std::optional<Value> body = {};          // the requestBody object
  std::vector<std::pair<std::string, Value>> responses = {};  // "200" -> response object
  bool bearer = false;                     // takes Auth: security bearer

  void parameter(std::string_view in, std::string_view name, bool required, Value schema);
  /// A successful response; `media` empty for one without a body.
  void respond(std::string code, std::string_view media = {}, std::optional<Value> schema = {});
  /// An error response (problem+json), described by its reason phrase and `when`.
  void error(int status, std::string_view when);
};

/// Describes a route's operation; reflect_routes() makes one per handler.
using Describe = void (*)(Operation&);

Value schema_ref(std::string_view component);
Value problem_schema();
/// `s`, or null.
Value nullable(Value s);

// ---- schemas -------------------------------------------------------------------

template <class T>
inline constexpr char key_of = 0;  // &key_of<T>: one address per type

consteval std::string component_name(std::meta::info t, bool qualified) {
  t = std::meta::dealias(t);
  std::string raw = !qualified && std::meta::has_identifier(t) && !std::meta::has_template_arguments(t)
                        ? std::string(std::meta::identifier_of(t))
                        : std::string(std::meta::display_string_of(t));
  std::string out;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == ':' && i + 1 < raw.size() && raw[i + 1] == ':') {
      out += '.';
      ++i;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.') {
      out += c;
    } else if (c != ' ' && (out.empty() || out.back() != '_')) {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

template <class I>
Value integer_schema() {
  Object o{{"type", std::string("integer")}};
  if constexpr (sizeof(I) <= 4) o.emplace_back("format", std::string(std::is_signed_v<I> ? "int32" : "uint32"));
  else o.emplace_back("format", std::string(std::is_signed_v<I> ? "int64" : "uint64"));
  if constexpr (sizeof(I) < 4 || (sizeof(I) == 4 && std::is_unsigned_v<I>)) {
    o.emplace_back("minimum", std::int64_t(std::numeric_limits<I>::min()));
    o.emplace_back("maximum", std::int64_t(std::numeric_limits<I>::max()));
  } else if constexpr (std::is_unsigned_v<I>) {
    o.emplace_back("minimum", std::int64_t(0));
  }
  return o;
}

/// A path capture, query parameter or header value (FromParam's types).
template <class T>
Value param_schema() {
  if constexpr (std::is_same_v<T, bool>) return Object{{"type", std::string("boolean")}};
  else if constexpr (std::is_integral_v<T>) return integer_schema<T>();
  else if constexpr (std::is_floating_point_v<T>) return Object{{"type", std::string("number")}};
  else return Object{{"type", std::string("string")}};
}

template <class T, json::detail::FieldOpts O = json::detail::FieldOpts{}>
Value schema(Components& c);

template <class T>
std::string struct_component(Components& c);

template <class D>
std::string duration_unit() {
  using P = typename D::period;
  if constexpr (std::is_same_v<P, std::nano>) return "nanoseconds";
  else if constexpr (std::is_same_v<P, std::micro>) return "microseconds";
  else if constexpr (std::is_same_v<P, std::milli>) return "milliseconds";
  else if constexpr (std::is_same_v<P, std::ratio<1>>) return "seconds";
  else if constexpr (std::is_same_v<P, std::ratio<60>>) return "minutes";
  else if constexpr (std::is_same_v<P, std::ratio<3600>>) return "hours";
  else if constexpr (std::is_same_v<P, std::ratio<86400>>) return "days";
  else return "units of " + std::to_string(P::num) + "/" + std::to_string(P::den) + " s";
}

template <class V, json::detail::FieldOpts O>
Value variant_schema(Components& c) {
  if constexpr (O.tag != nullptr) {
    // Internally tagged: {"<tag>": "<alternative>", ...its members}.
    Array one_of;
    Object mapping;
    [&]<class... A>(std::type_identity<std::variant<A...>>) {
      (
          [&] {
            static constexpr std::string_view name = std::define_static_string(json::detail::tag_name(^^A));
            auto component = struct_component<A>(c);
            Object tag_member{{std::string(O.tag), Object{{"const", std::string(name)}}}};
            one_of.push_back(Object{{"allOf", Array{schema_ref(component), Object{{"type", std::string("object")},
                                                                          {"properties", std::move(tag_member)},
                                                                          {"required", Array{std::string(O.tag)}}}}}});
            mapping.emplace_back(std::string(name), "#/components/schemas/" + component);
          }(),
          ...);
    }(std::type_identity<V>{});
    return Object{{"oneOf", std::move(one_of)},
                  {"discriminator", Object{{"propertyName", std::string(O.tag)}, {"mapping", std::move(mapping)}}}};
  } else {
    // Untagged: the first alternative that reads.
    Array any_of;
    [&]<class... A>(std::type_identity<std::variant<A...>>) {
      (any_of.push_back(schema<A, O>(c)), ...);
    }(std::type_identity<V>{});
    return Object{{"anyOf", std::move(any_of)}};
  }
}

template <class T, json::detail::FieldOpts O>
Value schema(Components& c) {
  using U = std::remove_cvref_t<T>;
  namespace jd = json::detail;
  if constexpr (std::is_same_v<U, bool>) {
    return Object{{"type", std::string("boolean")}};
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (O.as_string)  // written as "123", read from either
      return Object{{"type", std::string("string")},
                    {"format", std::string(std::is_signed_v<U> ? "int64" : "uint64")},
                    {"pattern", std::string(std::is_signed_v<U> ? "^-?[0-9]+$" : "^[0-9]+$")}};
    else return integer_schema<U>();
  } else if constexpr (std::is_floating_point_v<U>) {
    return Object{{"type", std::string("number")}, {"format", std::string(sizeof(U) == 4 ? "float" : "double")}};
  } else if constexpr (std::is_enum_v<U>) {
    static constexpr auto names = std::define_static_array(jd::enumerator_names(^^U));
    Array values;
    for (const char* n : names) values.push_back(std::string(n));
    return Object{{"type", std::string("string")}, {"enum", std::move(values)}};
  } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
    return Object{{"type", std::string("string")}};
  } else if constexpr (std::is_same_v<U, json::Value>) {
    return Object{};  // any JSON
  } else if constexpr (std::is_same_v<U, std::monostate>) {
    return Object{{"type", std::string("null")}};
  } else if constexpr (json::is_sys_time<U>::value) {
    if constexpr (O.time == jd::TimeFormat::unix_seconds)
      return Object{{"type", std::string("integer")}, {"format", std::string("int64")},
                    {"description", std::string("Unix time, in seconds")}};
    else if constexpr (O.time == jd::TimeFormat::unix_millis)
      return Object{{"type", std::string("integer")}, {"format", std::string("int64")},
                    {"description", std::string("Unix time, in milliseconds")}};
    else if constexpr (std::ratio_greater_equal_v<typename U::duration::period, std::chrono::days::period>)
      return Object{{"type", std::string("string")}, {"format", std::string("date")}};
    else return Object{{"type", std::string("string")}, {"format", std::string("date-time")}};
  } else if constexpr (std::is_same_v<U, std::chrono::year_month_day>) {
    return Object{{"type", std::string("string")}, {"format", std::string("date")}};
  } else if constexpr (json::is_duration<U>::value) {
    return Object{{"type", std::string(std::is_floating_point_v<typename U::rep> ? "number" : "integer")},
                  {"description", "A duration, in " + duration_unit<U>()}};
  } else if constexpr (json::is_optional<U>::value) {
    return nullable(schema<typename U::value_type, O>(c));
  } else if constexpr (json::is_vector<U>::value) {
    return Object{{"type", std::string("array")}, {"items", schema<typename U::value_type, O>(c)}};
  } else if constexpr (json::is_std_array<U>::value) {
    constexpr auto n = std::int64_t(std::tuple_size_v<U>);
    return Object{{"type", std::string("array")}, {"items", schema<typename U::value_type, O>(c)},
                  {"minItems", n}, {"maxItems", n}};
  } else if constexpr (json::is_string_map<U>::value) {
    return Object{{"type", std::string("object")}, {"additionalProperties", schema<typename U::mapped_type, O>(c)}};
  } else if constexpr (json::is_variant<U>::value) {
    return variant_schema<U, O>(c);
  } else if constexpr (json::ReflectableStruct<U>) {
    return schema_ref(struct_component<U>(c));
  } else {
    static_assert(false, "crocket::openapi: no JSON Schema for this type");
  }
}

/// A member's validation annotations, as JSON Schema keywords on `s` (the
/// schema of the value, or of what its optional holds).
template <std::meta::info M, class V>
Value constrain(Value s) {
  namespace jd = json::detail;
  Object extra;
  if constexpr (jd::has<json::Min>(M)) {
    constexpr auto v = jd::get<json::Min>(M);
    extra.emplace_back("minimum", v.integral ? Value(std::int64_t(v.i)) : Value(v.d));
  }
  if constexpr (jd::has<json::Max>(M)) {
    constexpr auto v = jd::get<json::Max>(M);
    extra.emplace_back("maximum", v.integral ? Value(std::int64_t(v.i)) : Value(v.d));
  }
  constexpr bool text = std::is_convertible_v<const V&, std::string_view>;
  constexpr bool map = json::is_string_map<V>::value;
  if constexpr (jd::has<json::MinLen>(M))
    extra.emplace_back(text ? "minLength" : map ? "minProperties" : "minItems", std::int64_t(jd::get<json::MinLen>(M).n));
  if constexpr (jd::has<json::MaxLen>(M))
    extra.emplace_back(text ? "maxLength" : map ? "maxProperties" : "maxItems", std::int64_t(jd::get<json::MaxLen>(M).n));
  if constexpr (jd::has<json::Pattern>(M)) extra.emplace_back("pattern", std::string(jd::get<json::Pattern>(M).source));
  if constexpr (jd::has<json::Email>(M)) extra.emplace_back("format", std::string("email"));
  if constexpr (jd::has<json::FromPath>(M)) extra.emplace_back("readOnly", true);
  if (extra.empty()) return s;
  Object o = s.is_object() ? s.as_object() : Object{};
  if (o.size() == 1 && o.front().first == "$ref") o = Object{{"allOf", Array{s}}};  // keywords beside $ref
  for (auto& kv : extra) o.push_back(std::move(kv));
  return o;
}

template <class T>
std::string struct_component(Components& c) {
  static constexpr std::string_view short_name = std::define_static_string(component_name(^^T, false));
  static constexpr std::string_view qualified = std::define_static_string(component_name(^^T, true));
  bool fresh = false;
  auto name = c.name(&key_of<T>, short_name, qualified, fresh);
  if (!fresh) return name;  // defined, or being defined (a recursive type)
  namespace jd = json::detail;
  Object properties;
  Array required;
  template for (constexpr auto m : std::define_static_array(jd::members_of(^^T))) {
    using M = [:std::meta::remove_cv(std::meta::type_of(m)):];
    constexpr std::string_view key = std::define_static_string(jd::json_name(m));
    Value s;
    if constexpr (json::is_optional<M>::value)
      s = nullable(constrain<m, typename M::value_type>(schema<typename M::value_type, jd::field_opts(m)>(c)));
    else s = constrain<m, M>(schema<M, jd::field_opts(m)>(c));
    properties.emplace_back(std::string(key), std::move(s));
    // What a client must send: json::read needs every member that is neither
    // optional nor has a default member initializer.
    if constexpr (!json::is_optional<M>::value && !std::meta::has_default_member_initializer(m))
      required.push_back(std::string(key));
  }
  Object o{{"type", std::string("object")}, {"properties", std::move(properties)}};
  if (!required.empty()) o.emplace_back("required", std::move(required));
  if constexpr (jd::has<json::DenyUnknownFields>(^^T)) o.emplace_back("additionalProperties", false);
  c.define(name, std::move(o));
  return name;
}

// ---- extractors and responders ------------------------------------------------------

/// What an extractor adds to an operation. Unknown extractors add nothing.
template <class T>
struct DescribeExtractor {
  static void apply(Operation&) {}
};
template <class T>
struct DescribeExtractor<Json<T>> {
  static void apply(Operation& op) {
    op.body = Object{{"required", true},
                     {"content", Object{{"application/json", Object{{"schema", schema<T>(op.components)}}}}}};
    op.error(415, "the body is not application/json");
    op.error(422, "the body is not valid JSON for this operation, or fails validation");
  }
};
template <class T>
struct DescribeExtractor<Query<T>> {
  static void apply(Operation& op) {
    template for (constexpr auto m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
      using M = [:std::meta::type_of(m):];
      constexpr std::string_view name = std::meta::identifier_of(m);
      if constexpr (json::is_optional<M>::value)
        op.parameter("query", name, false, param_schema<typename M::value_type>());
      else op.parameter("query", name, !std::meta::has_default_member_initializer(m), param_schema<M>());
    }
    op.error(422, "a query parameter is missing or not valid");
  }
};
template <FixedString Name, class T>
struct DescribeExtractor<Header<Name, T>> {
  static void apply(Operation& op) {
    if constexpr (json::is_optional<T>::value)
      op.parameter("header", Name.view(), false, param_schema<typename T::value_type>());
    else op.parameter("header", Name.view(), true, param_schema<T>());
    op.error(400, "the header is missing or not valid");
  }
};
template <>
struct DescribeExtractor<Auth> {
  static void apply(Operation& op) {
    op.bearer = true;
    op.error(401, "the bearer token is missing or not valid");
  }
};

template <class T>
struct is_with_status : std::false_type {};
template <int C, class T>
struct is_with_status<WithStatus<C, T>> : std::true_type {
  static constexpr int code = C;
  using type = T;
};
template <class T>
struct is_cacheable : std::false_type {};
template <class T>
struct is_cacheable<Cacheable<T>> : std::true_type {
  using type = T;
};
template <class T>
struct is_status : std::false_type {};
template <class T>
struct is_status<Status<T>> : std::true_type {
  using type = T;
};
template <class T>
struct is_expected : std::false_type {};
template <class T, class E>
struct is_expected<std::expected<T, E>> : std::true_type {
  using type = T;
};
template <class T>
struct is_json : std::false_type {};
template <class T>
struct is_json<Json<T>> : std::true_type {
  using type = T;
};

/// What a handler returning R sends on success. `code` is "200", "201", ... or
/// "2XX" when the status is chosen at run time.
template <class R>
void respond(Operation& op, std::string code = "200") {
  using U = std::remove_cvref_t<R>;
  namespace jd = json::detail;
  if constexpr (std::is_void_v<U> || std::is_same_v<U, NoContent>) {
    op.respond("204");
  } else if constexpr (Awaitable<U>) {
    respond<await_result_t<U>>(op, code);
  } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view> ||
                       std::is_same_v<U, const char*>) {
    op.respond(code, "text/plain", Value(Object{{"type", std::string("string")}}));
  } else if constexpr (is_json<U>::value) {
    op.respond(code, "application/json", schema<typename is_json<U>::type>(op.components));
  } else if constexpr (is_with_status<U>::value) {
    respond<typename is_with_status<U>::type>(op, std::to_string(is_with_status<U>::code));
  } else if constexpr (is_cacheable<U>::value) {
    respond<typename is_cacheable<U>::type>(op, code);
    op.respond("304");
  } else if constexpr (is_status<U>::value) {
    respond<typename is_status<U>::type>(op, "2XX");
  } else if constexpr (json::is_optional<U>::value) {
    respond<typename U::value_type>(op, code);
    op.error(404, "nothing found");
  } else if constexpr (is_expected<U>::value) {
    respond<typename is_expected<U>::type>(op, code);  // the errors: every operation's "default"
  } else if constexpr (std::is_same_v<U, ApiError>) {
    // only errors
  } else if constexpr (std::is_same_v<U, Response>) {
    op.respond("2XX");
  } else if constexpr (json::Encodable<U>) {
    op.respond(code, "application/json", schema<U>(op.components));
  } else {
    op.respond(code);  // a Responder of the application's own
  }
}

}  // namespace crocket::detail::openapi
