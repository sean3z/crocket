// Fuzz targets for crocket's parsers of untrusted input. Besides not crashing,
// each checks what must hold for every input it accepts: a document that
// parses writes out and parses back to the same text, a decoded message
// re-encodes stably, and so on.

#include "fuzz.hpp"

#include <crocket/crocket.hpp>

#include "router.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace crocket;

namespace {

// ---- json: any document ---------------------------------------------------------------

void json_value(std::string_view in) {
  auto v = json::parse(in);
  if (!v) return;
  std::string once, twice;
  json::dump(*v, once);
  auto again = json::parse(once);
  if (!again) fuzz::fail("a parsed document, written out, does not parse: " + once);
  json::dump(*again, twice);
  if (once != twice) fuzz::fail("a document does not write out the same twice: " + once + " vs " + twice);
}

fuzz::Register json_target{{
    .name = "json",
    .dict = {"true", "false", "null", "\"\\u", "\\\"", "1.5e-3", "-", "E+", "  ", "{\"a\":", "[[[[[[[[", "]]]]]]]]"},
    .run = json_value,
}};

// ---- json_typed: documents read into structs, with every kind of member ----------------

enum class Color { red, dark_green, blue [[= json::rename("BLUE")]] };
struct Circle { double r = 0; };
struct [[= json::rename("rect")]] Rect {
  double w = 0, h = 0;
};
struct Item {
  [[= json::min_len(1)]] std::string name;
  [[= json::min(1), = json::max(99)]] int qty = 1;
};
struct Checked {
  [[= json::email]] std::string email;
  [[= json::pattern("^[A-Z]{3}-[0-9]{4}$")]] std::string code;
  [[= json::min(0.5), = json::max(10.0)]] double weight = 1;
  [[= json::max_len(5)]] std::string nick = {};
  [[= json::min_len(1), = json::max_len(3)]] std::vector<Item> items;
};
struct Node;
struct AllOf { std::vector<Node> children; int all; };
struct AnyOf { std::vector<Node> children; int any; };
struct Node { std::variant<AllOf, AnyOf> op; };
struct Doc {
  [[= json::as_string]] std::uint64_t id = 0;
  std::string name = {};
  std::int32_t small = 0;
  std::uint8_t tiny = 0;
  double ratio = 0;
  bool flag = false;
  std::optional<std::int64_t> maybe = {};
  std::vector<std::string> tags = {};
  std::map<std::string, double> scores = {};
  Color color = Color::red;
  [[= json::tag("kind")]] std::optional<std::variant<Circle, Rect>> shape = {};
  std::variant<std::int64_t, std::string, bool> scalar = {};
  std::optional<std::chrono::sys_seconds> at = {};
  std::optional<std::chrono::year_month_day> day = {};
  std::optional<std::chrono::milliseconds> timeout = {};
  std::optional<Checked> checked = {};
  std::optional<Node> tree = {};
};

void json_typed(std::string_view in) {
  Doc d;
  if (!json::read(in, d)) return;
  auto once = json::to_string(d);
  Doc d2;
  if (auto ok = json::read(once, d2); !ok) fuzz::fail("a struct, written out, does not read back: " + once);
  auto twice = json::to_string(d2);
  if (once != twice) fuzz::fail("a struct does not write out the same twice: " + once + " vs " + twice);
}

fuzz::Register json_typed_target{{
    .name = "json_typed",
    .dict = {"\"id\":", "\"name\":", "\"small\":", "\"tiny\":", "\"ratio\":", "\"flag\":", "\"maybe\":", "\"tags\":",
             "\"scores\":", "\"color\":", "\"BLUE\"", "\"dark_green\"", "\"shape\":", "\"kind\":", "\"rect\"",
             "\"Circle\"", "\"scalar\":", "\"at\":", "\"2026-10-10T12:00:00Z\"", "\"day\":", "\"2026-02-29\"",
             "\"timeout\":", "\"checked\":", "\"email\":", "\"a@b.co\"", "\"code\":", "\"ABC-1234\"", "\"weight\":",
             "\"items\":", "\"qty\":", "\"tree\":", "\"op\":", "\"children\":", "\"all\":", "\"any\":", "null",
             "true"},
    .run = json_typed,
}};

// ---- proto: gRPC request messages --------------------------------------------------------

enum class Mood : std::uint8_t { calm, happy, grumpy };
enum class Tier { gold = 1, silver = 2 };
struct Address {
  std::string city;
  std::uint32_t zip = 0;
};
struct Hello {
  std::string name;
  std::int32_t age = 0;
  [[= proto::field(5)]] std::optional<std::string> nickname;
  std::vector<std::int64_t> lucky;
  Mood mood = Mood::calm;
  std::map<std::string, std::int32_t> scores;
  std::vector<Address> addresses;
  proto::Bytes avatar;
  double ratio = 0;
  bool admin = false;
  std::optional<Tier> tier;
  std::uint8_t level = 0;
  float f = 0;
  std::uint64_t big = 0;
  std::vector<double> packed;
};

void proto_message(std::string_view in) {
  auto m = proto::decode<Hello>(in);
  if (!m) return;
  auto once = proto::encode(*m);
  auto again = proto::decode<Hello>(once);
  if (!again) fuzz::fail("a decoded message, encoded, does not decode: " + again.error());
  if (proto::encode(*again) != once) fuzz::fail("a message does not encode the same twice");
}

fuzz::Register proto_target{{
    .name = "proto",
    .dict = {"\x0a", "\x10", "\x2a", "\x30", "\x38", "\x42", "\x4a", "\x52", "\x59", "\x60", "\x68", "\x70",
             "\x7d", "\x80\x01", "\x8a\x01", "\xff\xff\xff\xff\x0f", "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01",
             "\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x01"},
    .run = proto_message,
}};

// ---- query: query strings and percent-decoding -----------------------------------------------

void query(std::string_view in) {
  auto pairs = detail::parse_query(in);
  for (auto& [k, v] : pairs)
    if (k.size() + v.size() > in.size()) fuzz::fail("a decoded query pair is longer than the query");
  for (bool plus : {true, false})
    if (detail::percent_decode(in, plus).size() > in.size()) fuzz::fail("percent-decoding made the text longer");
}

fuzz::Register query_target{{
    .name = "query",
    .dict = {"%2", "%%", "%25", "%2B", "+", "&&", "==", "a=", "=b", "%C3%A9", "%FF", ";"},
    .run = query,
}};

// ---- route: paths against a route table, and route templates -----------------------------------

Routes& route_table() {
  static Routes r = [] {
    Routes out;
    int rank = 0;
    for (std::string_view p : {"/", "/users", "/users/{id}", "/users/{id}/orders", "/users/{id}/orders/{order}",
                               "/users/me", "/files/{a}/{b}/{c}", "/a/b/c/d/e/f/g/h", "/{x}", "/{x}/{y}",
                               "/api/v1/{resource}/{id}", "/api/v1/items/{id}", "/static/x.css"})
      for (auto m : {http::Method::Get, http::Method::Post, http::Method::Delete})
        out.push_back({.method = m, .path = std::string(p), .rank = rank++ % 3});
    return out;
  }();
  return r;
}

std::unique_ptr<detail::Router> g_router;

void route(std::string_view in) {
  for (auto m : {http::Method::Get, http::Method::Head, http::Method::Post, http::Method::Put}) {
    auto found = g_router->match(m, in);
    for (const auto* r : found) (void)found.captures(*r);
  }
  (void)g_router->allowed(in);
  // The same text as a template: a valid one with no captures matches itself.
  std::vector<http::Segment> segs;
  if (!http::parse_template(in, segs).empty()) return;
  // (ignite rejects a template deeper than the router goes)
  bool literal = segs.size() <= detail::Router::kMaxSegments && std::ranges::none_of(segs, [](auto& s) { return s.capture; });
  Routes one{{.method = http::Method::Get, .path = std::string(in)}};
  detail::Router single(one);
  if (literal && !in.empty() && in.front() == '/' && single.match(http::Method::Get, in).empty())
    fuzz::fail("a literal route does not match its own path");
}

fuzz::Register route_target{{
    .name = "route",
    .max_len = 512,
    .dict = {"/", "//", "{", "}", "{id}", "{x}", "users", "orders", "me", "..", ".", "%2F", "api", "v1"},
    .run = route,
    .setup = [] { g_router = std::make_unique<detail::Router>(route_table()); },
    .teardown = [] { g_router.reset(); },
}};

}  // namespace

// ---- request: the whole pipeline, from a request the engine could have parsed ---------------
// Input: "METHOD target\nname: value\n...\n\nbody". Header names starting with
// ':' set the connection instead: ":peer 10.0.0.5", ":tls 1", ":proto h2".

namespace fuzzapp {

struct Page {
  int page = 1;
  std::optional<std::string> q;
  bool exact = false;
};
struct Line {
  [[= json::min_len(1)]] std::string sku;
  [[= json::min(1)]] int qty = 1;
};
struct Order {
  std::uint64_t id = 0;
  [[= json::email]] std::string email;
  std::vector<Line> lines = {};
};
struct Hi {
  std::string name;
};
struct HiReply {
  std::string message;
};

namespace api {
[[= http::get("/users/{id}")]]
auto user(std::uint32_t id) -> std::string { return std::to_string(id); }
[[= http::get("/users/{id}/files/{name}")]]
auto file(std::uint64_t id, std::string name) -> std::string { return std::to_string(id) + name; }
[[= http::get("/search")]]
auto search(Query<Page> p) -> Json<Page> { return Json{p.value}; }
[[= http::post("/orders")]]
auto order(Json<Order> o) -> Json<Order> { return o; }
[[= http::put("/orders/{id}")]]
auto put(std::int64_t id, Header<"x-tenant"> tenant, Header<"x-page-size", std::optional<int>> size) -> std::string {
  return std::to_string(id) + std::string(tenant.value) + std::to_string(size.value.value_or(0));
}
[[= http::get("/me")]]
auto me(Auth a) -> std::string { return a.subject; }
[[= http::get("/doc")]]
auto doc() -> Cacheable<std::string> {
  return {std::string(1000, 'x'), {.last_modified = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000)),
                                   .ranges = true}};
}
[[= http::post("/raw")]]
auto raw(const Request& rq) -> std::string { return std::to_string(rq.body.size()); }
[[= http::get("/who")]]
auto who(Client c, RequestId id) -> std::string { return c.addr + std::string(c.scheme) + id.value; }
}  // namespace api

namespace greeter {
[[= grpc::rpc]]
auto say_hello(Hi h) -> HiReply { return {"hi " + h.name}; }
}  // namespace greeter

std::unique_ptr<Crocket> g_app;

void setup() {
  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  cfg.trusted_proxies = {"10.0.0.0/8", "fd00::/8"};
  cfg.allowed_hosts = {"example.com", "*.example.org", "localhost"};
  cfg.max_body_bytes = 64 << 10;
  g_app = std::make_unique<Crocket>(cfg);
  g_app->manage(Authenticator{[](std::string_view token) -> std::expected<Auth, ApiError> {
    if (token == "alice") return Auth{"alice", {}};
    return std::unexpected(ApiError::unauthorized("auth.invalid", "unknown token"));
  }});
  g_app->attach(Logger{})
      .attach(Metrics{})
      .attach(Cors::allow_origins({"https://app.example.com"}).allow_credentials())
      .mount("/", reflect_routes<^^api>())
      .mount("demo", reflect_routes<^^greeter>(), Mode::Grpc);
  if (auto ok = g_app->ignite(); !ok) fuzz::fail("the fuzz app did not ignite: " + ok.error().message());
}

void request(std::string_view in) {
  auto line_end = in.find('\n');
  std::string_view first = in.substr(0, line_end);
  std::string_view rest = line_end == std::string_view::npos ? std::string_view() : in.substr(line_end + 1);
  auto sp = first.find(' ');
  std::string_view method = first.substr(0, sp), target = sp == std::string_view::npos ? "/" : first.substr(sp + 1);
  if (method.empty() || target.empty() || target.front() != '/') return;  // h2o answers these itself

  Request rq;
  rq.method_text = std::string(method);
  rq.method = http::parse_method(rq.method_text);
  auto q = target.find('?');
  rq.path = detail::percent_decode(target.substr(0, q), false);
  if (q != std::string_view::npos) rq.query = detail::parse_query(target.substr(q + 1));
  rq.protocol = "http/1.1";
  rq.peer_addr = "203.0.113.7";

  auto body_at = rest.find("\n\n");
  std::string_view head = rest.substr(0, body_at);
  if (body_at != std::string_view::npos) rq.body = std::string(rest.substr(body_at + 2));
  while (!head.empty()) {
    auto nl = head.find('\n');
    std::string_view h = head.substr(0, nl);
    head = nl == std::string_view::npos ? std::string_view() : head.substr(nl + 1);
    if (h.starts_with(':')) {
      auto s2 = h.find(' ');
      std::string_view k = h.substr(0, s2), v = s2 == std::string_view::npos ? "" : h.substr(s2 + 1);
      if (k == ":peer") rq.peer_addr = std::string(v);
      else if (k == ":tls") rq.tls = v == "1";
      else if (k == ":proto") rq.protocol = v == "h2" ? "h2" : "http/1.1";
      continue;
    }
    auto colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;
    std::string_view k = h.substr(0, colon), v = h.substr(colon + 1);
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    // What h2o lets through: a valid name, and no CR, LF or NUL in the value.
    if (!http::valid_header_name(k) || v.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos) continue;
    rq.headers.add(k, v);
  }

  Response rs = g_app->handle(std::move(rq));
  if (rs.status < 100 || rs.status > 599) fuzz::fail("status " + std::to_string(rs.status));
  for (auto& [k, v] : rs.headers)
    if (!http::valid_header_name(k) || !http::valid_header_value(v))
      fuzz::fail("a response header not allowed in HTTP: " + k);
  if (auto ct = rs.headers.get("content-type"); ct && ct->starts_with("application/") && ct->find("json") != ct->npos &&
                                                   !rs.body.empty() && !json::parse(rs.body))
    fuzz::fail("a JSON response that does not parse: " + rs.body);
}

fuzz::Register request_target{{
    .name = "request",
    .dict = {"GET ", "POST ", "PUT ", "OPTIONS ", "HEAD ", "PATCH ", "BREW ", "\nhost: example.com",
             "\nhost: a.example.org", "\nx-forwarded-for: ", "\nforwarded: for=", "\nx-forwarded-proto: https",
             "\n:peer 10.1.2.3", "\n:peer ::1", "\n:tls 1", "\n:proto h2", "\nauthorization: Bearer alice",
             "\ncontent-type: application/json", "\ncontent-type: application/grpc", "\nte: trailers",
             "\ntraceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "\ngrpc-timeout: 100m",
             "\nx-request-id: ", "\nif-none-match: ", "\nif-modified-since: Tue, 14 Nov 2023 22:13:20 GMT",
             "\nrange: bytes=", "\nif-range: ", "\norigin: https://app.example.com",
             "\naccess-control-request-method: PUT", "\nx-tenant: t", "\nx-page-size: ", "\n\n", "?page=",
             "&q=", "&exact=true", "/users/", "/orders", "/search", "/doc", "/demo.Greeter/SayHello", "/who", "/me"},
    .run = request,
    .setup = setup,
    .teardown = [] { g_app.reset(); },
}};

}  // namespace fuzzapp
