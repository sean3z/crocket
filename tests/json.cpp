// JSON: the reader and writer, annotations, validation, and the problem+json
// errors that Json<T> bodies produce, in-process.

#include <crocket/crocket.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <variant>

using namespace crocket;
using namespace std::chrono_literals;

// ---- tiny harness --------------------------------------------------------------
static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) {                                                                    \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    ++g_checks;                                                                       \
    auto va_ = (a);                                                                   \
    auto vb_ = (b);                                                                   \
    if (!(va_ == vb_)) {                                                              \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (got %s)\n", __FILE__, __LINE__,  \
                   #a, #b, std::format("{}", va_).c_str());                           \
    }                                                                                 \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != hay.npos; }

/// The error from reading `text` as T ("" pointer and message on success).
template <class T>
json::Error error_of(std::string_view text, const json::ReadOptions& opts = {}) {
  T out{};
  auto r = json::read(text, out, opts);
  return r ? json::Error{json::Errc::syntax, "<no error>", "<none>"} : r.error();
}
template <class T>
bool reads(std::string_view text, const json::ReadOptions& opts = {}) {
  T out{};
  return json::read(text, out, opts).has_value();
}

// ---- types under test --------------------------------------------------------------

struct Address {
  std::string city;
  std::optional<std::string> zip;
};
struct Person {
  std::string name;
  int age = 0;
  std::vector<std::string> tags = {};
  Address address = {};
  std::map<std::string, double> scores = {};
  std::array<int, 2> pair = {};
};

struct Wide {
  std::uint64_t u;
  std::int64_t i;
  std::uint8_t small = 0;
};

struct [[= json::rename_all(json::camel_case)]] Camel {
  std::string display_name;
  int http_status = 0;
  [[= json::rename("ID")]] int id = 0;
};
struct [[= json::rename_all(json::snake_case)]] Snake {
  int userId = 0;
  int HTTPServerPort = 0;
};
struct [[= json::rename_all(json::kebab_case)]] Kebab { int max_age = 0; };
struct [[= json::rename_all(json::pascal_case)]] Pascal { int max_age = 0; };
struct [[= json::rename_all(json::screaming_snake_case)]] Screaming { int max_age = 0; };

struct [[= json::deny_unknown_fields]] Strict { int a = 0; };
struct Lenient { int a = 0; };

struct Optionals {
  [[= json::omit_null]] std::optional<int> gone;
  std::optional<int> shown;
};
struct [[= json::omit_null]] AllOmitted {
  std::optional<int> a;
  std::optional<std::string> b;
  int c = 1;
};

struct Ids {
  [[= json::as_string]] std::uint64_t id = 0;
  [[= json::as_string]] std::vector<std::int64_t> refs;
  [[= json::as_string]] std::optional<std::uint64_t> parent;
  std::uint64_t plain = 0;
};
struct [[= json::as_string]] AllStrings {
  std::int64_t a = 0;
  double b = 0;
  std::string c;
};

enum class Color { red, dark_green, blue [[= json::rename("BLUE")]] };
enum class [[= json::rename_all(json::camel_case)]] Access { read_only, read_write };
struct Paint {
  Color color = Color::red;
  std::vector<Access> modes = {};
};

struct Circle { double r = 0; };
struct [[= json::rename("rect")]] Rectangle {
  double w = 0, h = 0;
};
struct Drawing {
  [[= json::tag("kind")]] std::variant<Circle, Rectangle> shape;
  [[= json::tag("kind")]] std::optional<std::variant<Circle, Rectangle>> extra;
};

struct ByName { std::string name; };
struct ById { std::uint64_t id; };
struct Untagged {
  std::variant<std::int64_t, std::string, bool> scalar;
  std::variant<std::monostate, std::string> maybe;
  std::variant<ById, ByName> who;
};

// Both alternatives recurse into the same type: a document that fails deep down
// would be re-read 2^depth times without the backtracking budget.
struct Node;
struct AllOf { std::vector<Node> children; int all; };
struct AnyOf { std::vector<Node> children; int any; };
struct Node { std::variant<AllOf, AnyOf> op; };

struct Times {
  std::chrono::sys_seconds at;
  std::chrono::sys_time<std::chrono::milliseconds> ms;
  std::chrono::sys_days day;
  std::chrono::year_month_day ymd;
  std::chrono::milliseconds timeout{};
  [[= json::unix_seconds]] std::chrono::sys_seconds epoch;
  [[= json::unix_millis]] std::chrono::sys_time<std::chrono::milliseconds> epoch_ms;
};

struct Item {
  [[= json::min_len(1)]] std::string name;
  [[= json::min(1), = json::max(99)]] int qty = 1;
};
struct Order {
  [[= json::email]] std::string email;
  [[= json::pattern("^[A-Z]{3}-[0-9]{4}$")]] std::string code;
  [[= json::min(0.5), = json::max(10.0)]] double weight = 1;
  [[= json::max_len(5)]] std::string nick = {};
  [[= json::min_len(1), = json::max_len(3)]] std::vector<Item> items;
  [[= json::min(18)]] std::optional<int> age;
};

struct Doc {
  json::Value any;
};

// ---- an app, for the HTTP side ------------------------------------------------------

namespace jsonapi {
[[= http::post("/orders")]]
auto create(Json<Order> o) -> Json<Order> { return o; }
[[= http::post("/strict")]]
auto strict(Json<Lenient> l) -> Json<Lenient> { return l; }
}  // namespace jsonapi

int main() {
  section("round trip");
  {
    Person p{"Ada", 36, {"math", "engines"}, {"London", std::nullopt}, {{"x", 1.5}}, {1, 2}};
    auto text = json::to_string(p);
    CHECK_EQ(text, std::string(R"({"name":"Ada","age":36,"tags":["math","engines"],"address":{"city":"London","zip":null},"scores":{"x":1.5},"pair":[1,2]})"));
    auto back = json::from_string<Person>(text);
    CHECK(back.has_value());
    CHECK_EQ(back->name, std::string("Ada"));
    CHECK_EQ(back->tags.size(), std::size_t(2));
    CHECK_EQ(back->scores["x"], 1.5);
    CHECK_EQ(back->pair[1], 2);
    CHECK(!back->address.zip);
    // Whitespace, escapes, and members in any order.
    auto spaced = json::from_string<Person>(
        " {\n \"tags\" : [ ] , \"address\":{\"city\":\"Z\\u00fcrich\\n\"}, \"name\":\"\\\"A\\\"\"} ");
    CHECK(spaced.has_value());
    CHECK_EQ(spaced->address.city, std::string("Z\xC3\xBCrich\n"));
    CHECK_EQ(spaced->name, std::string("\"A\""));
    // std::array needs exactly N elements.
    CHECK(contains(error_of<Person>(R"({"name":"a","address":{"city":"c"},"pair":[1]})").message, "array of 2"));
    CHECK(contains(error_of<Person>(R"({"name":"a","address":{"city":"c"},"pair":[1,2,3]})").message, "array of 2"));
  }

  section("errors name the field and the position");
  {
    auto e = error_of<Person>(R"({"name":"a","address":{"city":7}})");
    CHECK(e.code == json::Errc::type);
    CHECK_EQ(e.pointer, std::string("/address/city"));
    CHECK_EQ(e.describe(), std::string("field '/address/city': expected string, got number"));
    auto missing = error_of<Person>(R"({"address":{"city":"c"}})");
    CHECK_EQ(missing.describe(), std::string("field '/name': required field is missing"));
    auto inner = error_of<Person>(R"({"name":"a","address":{"city":"c"},"tags":["x",3]})");
    CHECK_EQ(inner.pointer, std::string("/tags/1"));
    auto syntax = error_of<Person>("{\n  \"name\": \"a\",\n  \"age\": tru\n}");
    CHECK(syntax.code == json::Errc::syntax);
    CHECK_EQ(syntax.describe(), std::string("malformed JSON at line 3, column 10: invalid literal"));
    CHECK(error_of<Person>(R"({"name":"a"} x)").message == "unexpected trailing characters");
    CHECK(error_of<Person>("").message == "unexpected end of input");
    CHECK(error_of<Person>(R"({"name":"a",})").code == json::Errc::syntax);
    CHECK(error_of<Person>(R"([1,])").code == json::Errc::syntax);  // malformed beats the wrong type
    CHECK_EQ(error_of<Person>("[1]").describe(), std::string("body: expected object, got array"));
    // Map keys are escaped in pointers (RFC 6901).
    struct Keyed { std::map<std::string, int> m; };
    CHECK_EQ(error_of<Keyed>(R"({"m":{"a/b~c":"x"}})").pointer, std::string("/m/a~1b~0c"));
  }

  section("integers: the full range, no rounding");
  {
    auto w = json::from_string<Wide>(R"({"u":18446744073709551615,"i":-9223372036854775808})");
    CHECK(w.has_value());
    CHECK_EQ(w->u, std::numeric_limits<std::uint64_t>::max());
    CHECK_EQ(w->i, std::numeric_limits<std::int64_t>::min());
    CHECK_EQ(json::to_string(*w), std::string(R"({"u":18446744073709551615,"i":-9223372036854775808,"small":0})"));
    CHECK_EQ(error_of<Wide>(R"({"u":18446744073709551616,"i":0})").message, std::string("integer out of range"));
    CHECK_EQ(error_of<Wide>(R"({"u":-1,"i":0})").message, std::string("integer out of range"));
    CHECK(reads<Wide>(R"({"u":-0,"i":0})"));
    CHECK_EQ(error_of<Wide>(R"({"u":1,"i":0,"small":256})").message, std::string("integer out of range"));
    CHECK_EQ(error_of<Wide>(R"({"u":1.5,"i":0})").message, std::string("expected integer, got 1.5"));
    CHECK_EQ(error_of<Wide>(R"({"u":1e3,"i":0})").message, std::string("expected integer, got 1e3"));
  }

  section("integers as strings, for JavaScript");
  {
    Ids ids{9007199254740993ULL, {-1, 2}, 7, 9007199254740993ULL};
    CHECK_EQ(json::to_string(ids),
             std::string(R"({"id":"9007199254740993","refs":["-1","2"],"parent":"7","plain":9007199254740993})"));
    auto back = json::from_string<Ids>(R"({"id":"9007199254740993","refs":["-1",2],"parent":null})");
    CHECK(back.has_value());
    CHECK_EQ(back->id, 9007199254740993ULL);
    CHECK_EQ(back->refs[1], std::int64_t(2));
    CHECK(!back->parent);
    CHECK_EQ(error_of<Ids>(R"({"id":"12a"})").message, std::string("expected integer, got \"12a\""));
    CHECK(error_of<Ids>(R"({"id":" 1"})").code == json::Errc::type);
    CHECK_EQ(error_of<Ids>(R"({"plain":"1"})").message, std::string("expected integer, got string"));
    // On a struct: every integer member; others are untouched.
    CHECK_EQ(json::to_string(AllStrings{5, 1.5, "x"}), std::string(R"({"a":"5","b":1.5,"c":"x"})"));
  }

  section("UTF-8 is checked on input and repaired on output");
  {
    struct S { std::string s; };
    CHECK(reads<S>("{\"s\":\"caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x80\"}"));
    for (std::string_view bad : {
             std::string_view("{\"s\":\"\x80\"}"),               // lone continuation byte
             std::string_view("{\"s\":\"\xC0\xAF\"}"),           // overlong '/'
             std::string_view("{\"s\":\"\xE0\x80\xAF\"}"),       // overlong, three bytes
             std::string_view("{\"s\":\"\xED\xA0\x80\"}"),       // UTF-16 surrogate
             std::string_view("{\"s\":\"\xF4\x90\x80\x80\"}"),   // above U+10FFFF
             std::string_view("{\"s\":\"\xE2\x82\"}"),           // truncated
             std::string_view("{\"s\":\"\xFF\"}"),
         }) {
      auto e = error_of<S>(bad);
      CHECK(e.code == json::Errc::syntax);
      CHECK_EQ(e.message, std::string("invalid UTF-8 in string"));
    }
    CHECK_EQ(error_of<S>(R"({"s":"\ud800"})").message, std::string("unpaired surrogate"));
    CHECK_EQ(error_of<S>(R"({"s":"\udc00"})").message, std::string("unpaired surrogate"));
    CHECK_EQ(json::from_string<S>(R"({"s":"🚀"})")->s, std::string("\xF0\x9F\x9A\x80"));
    CHECK_EQ(error_of<S>("{\"s\":\"a\tb\"}").message, std::string("control character in string"));
    // Keys are checked too, and so are values that are skipped.
    CHECK(error_of<S>("{\"s\":\"\",\"\xC0\xAF\":1}").code == json::Errc::syntax);
    CHECK(error_of<S>("{\"s\":\"\",\"x\":[\"\xFF\"]}").code == json::Errc::syntax);

    CHECK_EQ(json::to_string(std::string("ok \xC3\xA9 bad \xFF\xC0\xAF end")),
             std::string("\"ok \xC3\xA9 bad \xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD end\""));
    CHECK_EQ(json::to_string(std::string("q\"\\\n\x01\x7F")), std::string(R"("q\"\\\n\u0001)" "\x7F\""));
  }

  section("duplicate keys are rejected everywhere");
  {
    auto e = error_of<Lenient>(R"({"a":1,"a":2})");
    CHECK(e.code == json::Errc::duplicate_key);
    CHECK_EQ(e.message, std::string("duplicate key \"a\""));
    // The same key spelled with an escape is the same key.
    CHECK(error_of<Lenient>(R"({"a":1,"a":2})").code == json::Errc::duplicate_key);
    CHECK(error_of<Lenient>(R"({"x":1,"x":2})").code == json::Errc::duplicate_key);         // unknown member
    CHECK(error_of<Lenient>(R"({"x":{"y":1,"y":2}})").code == json::Errc::duplicate_key);   // in a skipped value
    struct Counts { std::map<std::string, int> m; };
    CHECK(error_of<Counts>(R"({"m":{"k":1,"k":2}})").code == json::Errc::duplicate_key);
    CHECK(error_of<Doc>(R"({"any":{"k":1,"k":2}})").code == json::Errc::duplicate_key);
    CHECK(!json::parse(R"([{"k":1,"k":2}])"));
    // Many distinct keys: the set switches from a list to a hash.
    std::string many = "{";
    for (int i = 0; i < 40; ++i) many += std::format("\"k{}\":{},", i, i);
    CHECK(reads<Lenient>(many + "\"a\":1}"));
    CHECK(error_of<Lenient>(many + "\"k39\":1}").code == json::Errc::duplicate_key);
  }

  section("limits");
  {
    json::ReadOptions small{.max_depth = 3, .max_string_bytes = 8, .max_object_members = 3, .max_array_elements = 4};
    CHECK(reads<json::Value>("[[[1]]]", small));
    auto deep = error_of<json::Value>("[[[[1]]]]", small);
    CHECK(deep.code == json::Errc::limit);
    CHECK_EQ(deep.message, std::string("nested deeper than 3 levels"));
    CHECK(error_of<json::Value>(R"("123456789")", small).code == json::Errc::limit);
    CHECK(reads<json::Value>(R"("12345678")", small));
    CHECK(error_of<json::Value>(R"("1234567\n9")", small).code == json::Errc::limit);  // counted after unescaping
    CHECK(error_of<json::Value>(R"({"123456789":1})", small).code == json::Errc::limit);   // keys too
    CHECK(error_of<json::Value>(R"({"a":1,"b":2,"c":3,"d":4})", small).code == json::Errc::limit);
    CHECK(error_of<json::Value>("[1,2,3,4,5]", small).code == json::Errc::limit);
    CHECK(error_of<Lenient>(R"({"x":[1,2,3,4,5]})", small).code == json::Errc::limit);  // skipped values too
    struct V { std::vector<int> v; };
    CHECK_EQ(error_of<V>(R"({"v":[1,2,3,4,5]})", small).pointer, std::string("/v"));
    // The default depth stops a stack-exhausting document.
    std::string bomb(100000, '[');
    CHECK(error_of<json::Value>(bomb).code == json::Errc::limit);
    CHECK(error_of<Lenient>("{\"x\":" + bomb).code == json::Errc::limit);
  }

  section("names: rename and rename_all");
  {
    CHECK_EQ(json::to_string(Camel{"Ada", 200, 7}), std::string(R"({"displayName":"Ada","httpStatus":200,"ID":7})"));
    CHECK(json::from_string<Camel>(R"({"displayName":"x","ID":3})")->id == 3);
    CHECK_EQ(json::to_string(Snake{1, 2}), std::string(R"({"user_id":1,"http_server_port":2})"));
    CHECK_EQ(json::to_string(Kebab{1}), std::string(R"({"max-age":1})"));
    CHECK_EQ(json::to_string(Pascal{1}), std::string(R"({"MaxAge":1})"));
    CHECK_EQ(json::to_string(Screaming{1}), std::string(R"({"MAX_AGE":1})"));
    CHECK(error_of<Camel>(R"({"display_name":"x"})").pointer == "/displayName");  // the C++ name is not accepted
  }

  section("unknown fields");
  {
    CHECK(reads<Lenient>(R"({"a":1,"b":{"c":[1,2]}})"));
    auto e = error_of<Strict>(R"({"a":1,"b":2})");
    CHECK(e.code == json::Errc::unknown_field);
    CHECK_EQ(e.pointer, std::string("/b"));
    CHECK_EQ(e.message, std::string("unknown field \"b\""));
    CHECK(error_of<Lenient>(R"({"b":2})", {.deny_unknown_fields = true}).code == json::Errc::unknown_field);
  }

  section("leaving out empty optionals");
  {
    CHECK_EQ(json::to_string(Optionals{}), std::string(R"({"shown":null})"));
    CHECK_EQ(json::to_string(Optionals{1, 2}), std::string(R"({"gone":1,"shown":2})"));
    CHECK_EQ(json::to_string(AllOmitted{}), std::string(R"({"c":1})"));
    CHECK_EQ(json::to_string(AllOmitted{std::nullopt, "x", 2}), std::string(R"({"b":"x","c":2})"));
  }

  section("enums as strings");
  {
    CHECK_EQ(json::to_string(Paint{Color::dark_green, {Access::read_only, Access::read_write}}),
             std::string(R"({"color":"dark_green","modes":["readOnly","readWrite"]})"));
    CHECK_EQ(json::to_string(Color::blue), std::string(R"("BLUE")"));
    CHECK(json::from_string<Paint>(R"({"color":"BLUE"})")->color == Color::blue);
    auto e = error_of<Paint>(R"({"color":"blue"})");
    CHECK_EQ(e.message, std::string(R"(expected one of "red", "dark_green", "BLUE")"));
    CHECK_EQ(error_of<Paint>(R"({"color":2})").message, std::string("expected string, got number"));
    CHECK_EQ(json::to_string(static_cast<Color>(7)), std::string("7"));  // not a named value
  }

  section("tagged unions");
  {
    Drawing d{Rectangle{2, 3}, Circle{1}};
    CHECK_EQ(json::to_string(d), std::string(R"({"shape":{"kind":"rect","w":2,"h":3},"extra":{"kind":"Circle","r":1}})"));
    auto back = json::from_string<Drawing>(R"({"shape":{"w":4,"h":5,"kind":"rect"}})");
    CHECK(back.has_value());
    CHECK(std::get<Rectangle>(back->shape).h == 5);
    CHECK(!back->extra);
    CHECK(std::holds_alternative<Circle>(json::from_string<Drawing>(R"({"shape":{"kind":"Circle","r":2}})")->shape));
    auto unknown = error_of<Drawing>(R"({"shape":{"kind":"hexagon"}})");
    CHECK_EQ(unknown.pointer, std::string("/shape/kind"));
    CHECK_EQ(unknown.message, std::string(R"(unknown "kind" "hexagon"; expected one of "Circle", "rect")"));
    CHECK_EQ(error_of<Drawing>(R"({"shape":{"r":2}})").message, std::string(R"(missing member "kind")"));
    CHECK(error_of<Drawing>(R"({"shape":{"kind":"Circle","kind":"rect"}})").code == json::Errc::duplicate_key);
    CHECK_EQ(error_of<Drawing>(R"({"shape":{"kind":"Circle","r":"x"}})").pointer, std::string("/shape/r"));
  }

  section("untagged variants");
  {
    auto u = json::from_string<Untagged>(R"({"scalar":"s","maybe":null,"who":{"name":"ada"}})");
    CHECK(u.has_value());
    CHECK(std::get<std::string>(u->scalar) == "s");
    CHECK(std::holds_alternative<std::monostate>(u->maybe));
    CHECK(std::get<ByName>(u->who).name == "ada");
    u = json::from_string<Untagged>(R"({"scalar":5,"maybe":"x","who":{"id":7}})");
    CHECK(std::get<std::int64_t>(u->scalar) == 5);
    CHECK(std::get<ById>(u->who).id == 7);
    CHECK(std::get<bool>(json::from_string<Untagged>(R"({"scalar":true,"maybe":null,"who":{"id":1}})")->scalar));
    CHECK_EQ(json::to_string(*u), std::string(R"({"scalar":5,"maybe":"x","who":{"id":7}})"));
    CHECK_EQ(error_of<Untagged>(R"({"scalar":[],"maybe":null,"who":{"id":1}})").message,
             std::string("expected boolean, number or string, got array"));
    CHECK(contains(error_of<Untagged>(R"({"scalar":1,"maybe":null,"who":{"nope":1}})").message,
                   "matches no alternative"));
  }

  section("untagged variants cannot be made to backtrack forever");
  {
    CHECK(reads<Node>(R"({"op":{"children":[{"op":{"children":[],"any":1}}],"all":1}})"));
    std::string evil;
    for (int i = 0; i < 20; ++i) evil += R"({"op":{"children":[)";  // depth 62 of 64
    evil += R"({"op":{"children":[]}})";  // neither "all" nor "any": both alternatives fail
    for (int i = 0; i < 20; ++i) evil += "]}}";
    auto t0 = std::chrono::steady_clock::now();
    auto e = error_of<Node>(evil);
    CHECK(e.code == json::Errc::limit);
    CHECK(contains(e.message, "tag it with json::tag"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    // A syntax error inside an alternative is reported as one, not as "no alternative".
    CHECK(error_of<Untagged>("{\"scalar\":1,\"maybe\":null,\"who\":{\"id\":1,\"x\":\"\xFF\"}}").code ==
          json::Errc::syntax);
  }

  section("dates and times");
  {
    using namespace std::chrono;
    Times t{sys_days(2026y / October / 5) + 14h + 3min + 7s,
            sys_days(2026y / October / 5) + 14h + 3min + 7s + 250ms,
            sys_days(2026y / February / 28),
            2024y / February / 29,
            1500ms,
            sys_seconds(1'700'000'000s),
            sys_time<milliseconds>(1'700'000'000'123ms)};
    auto text = json::to_string(t);
    CHECK_EQ(text, std::string(R"({"at":"2026-10-05T14:03:07Z","ms":"2026-10-05T14:03:07.250Z","day":"2026-02-28",)"
                               R"("ymd":"2024-02-29","timeout":1500,"epoch":1700000000,"epoch_ms":1700000000123})"));
    auto back = json::from_string<Times>(text);
    CHECK(back.has_value());
    CHECK(back->at == t.at && back->ms == t.ms && back->day == t.day && back->ymd == t.ymd);
    CHECK(back->timeout == 1500ms && back->epoch == t.epoch && back->epoch_ms == t.epoch_ms);

    auto at = [](std::string_view s) {
      struct T { sys_time<milliseconds> at; };
      auto r = json::from_string<T>(std::format(R"({{"at":"{}"}})", s));
      return r ? std::optional(r->at) : std::nullopt;
    };
    auto ref = sys_days(2026y / October / 5) + 12h;
    CHECK(at("2026-10-05T14:00:00+02:00") == ref);
    CHECK(at("2026-10-05t07:30:00-04:30") == ref);
    CHECK(at("2026-10-05 12:00:00z") == ref);
    CHECK(at("2026-10-05T12:00:00.0019999Z") == ref + 1ms);  // extra digits truncate
    CHECK(at("1969-12-31T23:59:59.5Z") == sys_days(1970y / January / 1) - 500ms);
    for (auto bad : {"2026-02-30T00:00:00Z", "2026-10-05T24:00:00Z", "2026-10-05T23:59:60Z", "2026-10-05T12:00:00",
                     "2026-10-05T12:00Z", "2026-10-05T12:00:00.Z", "2026-10-05T12:00:00+2:00", "20261005T120000Z",
                     "2026-10-05T12:00:00Z ", "2026-10-05"})
      CHECK(!at(bad));
    struct D { year_month_day d; };
    CHECK(!reads<D>(R"({"d":"2023-02-29"})"));
    CHECK(!reads<D>(R"({"d":"2023-1-01"})"));
    // Out of range for the member's duration: nanoseconds end in 2262.
    struct N { sys_time<nanoseconds> t; };
    CHECK_EQ(error_of<N>(R"({"t":"2300-01-01T00:00:00Z"})").message, std::string("timestamp out of range"));
    CHECK(reads<N>(R"({"t":"2200-01-01T00:00:00.123456789Z"})"));
    CHECK_EQ(json::to_string(N{sys_days(2000y / January / 1) + 5ns}), std::string(R"({"t":"2000-01-01T00:00:00.000000005Z"})"));
  }

  section("validation reports every failing field");
  {
    auto ok = json::from_string<Order>(
        R"({"email":"ada@example.com","code":"ABC-1234","nick":"héllo","items":[{"name":"x","qty":2}]})");
    CHECK(ok.has_value());
    auto e = error_of<Order>(
        R"({"email":"nope","code":"abc-1234","weight":11,"nick":"toolong","items":[{"name":"x"},{"name":"","qty":0}],"age":17})");
    CHECK(e.code == json::Errc::validation);
    CHECK_EQ(e.errors.size(), std::size_t(7));
    auto has = [&](std::string_view ptr, std::string_view detail) {
      for (auto& f : e.errors)
        if (f.pointer == ptr && f.detail == detail) return true;
      std::fprintf(stderr, "    missing %.*s: %.*s\n", int(ptr.size()), ptr.data(), int(detail.size()), detail.data());
      return false;
    };
    CHECK(has("/email", "must be an email address"));
    CHECK(has("/code", "must match the pattern ^[A-Z]{3}-[0-9]{4}$"));
    CHECK(has("/weight", "must be at most 10"));
    CHECK(has("/nick", "must have at most 5 characters"));
    CHECK(has("/items/1/name", "must have at least 1 character"));
    CHECK(has("/items/1/qty", "must be at least 1"));
    CHECK(has("/age", "must be at least 18"));
    CHECK(contains(e.describe(), "field '/email': must be an email address; field '/code': "));
    // A type error still stops reading: the document is not what was described.
    CHECK(error_of<Order>(R"({"email":"nope","code":5})").code == json::Errc::type);
    // Lengths of arrays.
    auto many = error_of<Order>(R"({"email":"a@b.co","code":"ABC-1234","items":[]})");
    CHECK(many.errors.size() == 1 && many.errors[0].detail == "must have at least 1 element");
    // The same checks on a value built in code.
    Order built{"x@y.org", "XYZ-0001", 0.25, "", {}, 30};
    auto errs = json::validate(built);
    CHECK_EQ(errs.size(), std::size_t(2));
    CHECK(errs.size() == 2 && errs[0].pointer == "/weight" && errs[1].pointer == "/items");
    CHECK(errs.size() == 2 && errs[0].detail == "must be at least 0.5");
  }

  section("email addresses");
  {
    using json::detail::valid_email;
    for (auto good : {"a@b.co", "first.last+tag@sub.example.org", "o'brien@example.ie", "x@xn--bcher-kva.example",
                      "\xC3\xBC@m\xC3\xBCnchen.de"})
      CHECK(valid_email(good));
    for (auto bad : {"", "@b.co", "a@", "a@b", "a@@b.co", "a.@b.co", ".a@b.co", "a..b@c.co", "a b@c.co", "a@-b.co",
                     "a@b-.co", "a@b..co", "a@b.123", "a@b.co.", "\"q\"@b.co"})
      CHECK(!valid_email(bad));
    CHECK(!valid_email(std::string(65, 'a') + "@b.co"));
    CHECK(!valid_email(std::string("a\0b@c.co", 8)));
  }

  section("patterns");
  {
    auto match = []<json::Pattern P>(std::string_view s) { return crocket::detail::regex::search(P.program, s); };
    CHECK((match.operator()<json::pattern("^a+b?$")>("aaab")));
    CHECK((!match.operator()<json::pattern("^a+b?$")>("aaabb")));
    CHECK((match.operator()<json::pattern("b")>("abc")));  // a search, not a full match
    CHECK((match.operator()<json::pattern("^(cat|dog)s?$")>("dogs")));
    CHECK((!match.operator()<json::pattern("^(cat|dog)s?$")>("cow")));
    CHECK((match.operator()<json::pattern("^[^0-9\\s]{2,3}$")>("ab")));
    CHECK((!match.operator()<json::pattern("^[^0-9\\s]{2,3}$")>("a1")));
    CHECK((!match.operator()<json::pattern("^[^0-9\\s]{2,3}$")>("abcd")));
    CHECK((match.operator()<json::pattern("^\\d{4}-\\d{2}$")>("2026-10")));
    CHECK((match.operator()<json::pattern("^\\w+\\.\\w+$")>("a_1.b")));
    CHECK((match.operator()<json::pattern("^.$")>("\xC3\xA9")));        // . is one code point
    CHECK((match.operator()<json::pattern("^[à-ÿ]+$")>("\xC3\xA9\xC3\xA8")));
    CHECK((match.operator()<json::pattern("^\\u00e9$")>("\xC3\xA9")));
    CHECK((match.operator()<json::pattern("^a{2,}$")>("aaaa")));
    CHECK((!match.operator()<json::pattern("^a{2,}$")>("a")));
    CHECK((match.operator()<json::pattern("^(?:ab)*$")>("")));
    CHECK((match.operator()<json::pattern("^[-a]+$")>("-a-")));
    CHECK((match.operator()<json::pattern("^\\$\\.\\[$")>("$.[")));
    CHECK((!match.operator()<json::pattern("^\\D$")>("5")));
    // Catastrophic backtracking for std::regex; linear here.
    std::string evil(100000, 'a');
    auto t0 = std::chrono::steady_clock::now();
    CHECK((!match.operator()<json::pattern("^(a*)*b$")>(evil)));
    CHECK((!match.operator()<json::pattern("^(a|aa)+$")>(evil + "!")));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
  }

  section("json::Value");
  {
    auto v = json::parse(R"({"a":[1,-2.5,"x",true,null,{"b":{}}],"big":18446744073709551615})");
    CHECK(v.has_value());
    CHECK(v->find("a")->as_array().size() == 6);
    CHECK(v->find("a")->as_array()[0].as_int() == 1);
    CHECK(v->find("big")->is_number() && !v->find("big")->is_int());
    std::string out;
    json::dump(*v, out);
    CHECK_EQ(out, std::string(R"({"a":[1,-2.5,"x",true,null,{"b":{}}],"big":18446744073709551616})"));
    auto d = json::from_string<Doc>(R"({"any":[1,{"k":"v"}]})");
    CHECK(d.has_value() && d->any.is_array());
    CHECK_EQ(json::to_string(*d), std::string(R"({"any":[1,{"k":"v"}]})"));
    Wide w{};
    CHECK(json::decode(json::parse(R"({"u":1,"i":2})").value(), w).has_value() && w.i == 2);
  }

  section("Json<T> bodies: problem+json errors");
  {
    Crocket app;
    app.mount("/", reflect_routes<^^jsonapi>());
    LocalClient client(app);
    auto ok = client.post("/orders").json(R"({"email":"a@b.co","code":"ABC-1234","items":[{"name":"x"}]})").dispatch();
    CHECK_EQ(ok.status, 200);
    CHECK(contains(ok.body, R"("items":[{"name":"x","qty":1}])"));

    auto bad = client.post("/orders").json(R"({"email":"a@b","code":"ABC-1234","items":[{"name":""}]})").dispatch();
    CHECK_EQ(bad.status, 422);
    CHECK_EQ(bad.error_code, std::string_view("json.validation"));
    CHECK_EQ(*bad.headers.get("content-type"), std::string_view("application/problem+json"));
    CHECK(contains(bad.body, R"({"title":"Unprocessable Content","status":422,"detail":"field '/email': must be an email address; field '/items/0/name': must have at least 1 character","code":"json.validation","request_id":")"));
    CHECK(contains(bad.body, R"("errors":[{"pointer":"/email","detail":"must be an email address"},{"pointer":"/items/0/name","detail":"must have at least 1 character"}]})"));

    auto dup = client.post("/strict").json(R"({"a":1,"a":2})").dispatch();
    CHECK_EQ(dup.error_code, std::string_view("json.duplicate_key"));
    auto utf = client.post("/strict").json("{\"\xFF\":1}").dispatch();
    CHECK_EQ(utf.error_code, std::string_view("json.invalid"));
    CHECK(contains(utf.body, "invalid UTF-8"));
    CHECK(!contains(utf.body, R"("errors")"));  // a syntax error has no field

    Crocket strict_app{Config{.json = {.max_depth = 2, .deny_unknown_fields = true}}};
    strict_app.mount("/", reflect_routes<^^jsonapi>());
    LocalClient strict(strict_app);
    auto unknown = strict.post("/strict").json(R"({"a":1,"b":2})").dispatch();
    CHECK_EQ(unknown.error_code, std::string_view("json.unknown_field"));
    CHECK(contains(unknown.body, R"("errors":[{"pointer":"/b","detail":"unknown field \"b\""}])"));
    auto deep = strict.post("/orders").json(R"({"items":[{"name":"x"}]})").dispatch();
    CHECK_EQ(deep.error_code, std::string_view("json.limit_exceeded"));
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
