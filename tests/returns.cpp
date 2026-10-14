// Plain return types: a struct T, std::optional<T>, std::vector<T> and
// Created<T> are JSON on HTTP routes and protobuf on gRPC routes; Json<T> and
// Proto<T> choose explicitly. Also what OpenAPI says about each.

#include <crocket/crocket.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace crocket;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != hay.npos; }

struct Hero {
  std::int64_t id;
  std::string name;
  std::string alias;
};
struct HeroId {
  std::int64_t id;
};

static Hero flash() { return {1, "Barry Allen", "The Flash"}; }

namespace web {
[[= http::get("/plain")]]
Hero plain() { return flash(); }
[[= http::get("/maybe/{id}")]]
std::optional<Hero> maybe(std::int64_t id) {
  if (id == 1) return flash();
  return std::nullopt;
}
[[= http::get("/list")]]
std::vector<Hero> list() { return {flash(), {2, "Dick Grayson", "Robin"}}; }
[[= http::post("/created")]]
Created<Hero> created() { return {{3, "Tim Drake", "Robin"}, "/heroes/3"}; }
[[= http::get("/explicit-json")]]
Json<Hero> explicit_json() { return {flash()}; }
[[= http::get("/explicit-proto")]]
Proto<Hero> explicit_proto() { return {flash()}; }
[[= http::get("/text")]]
std::string text() { return "plain text"; }
[[= http::get("/expected")]]
Result<Hero> expected() { return flash(); }
}  // namespace web

namespace heroes {
[[= grpc::rpc]]
Hero get_hero(HeroId req) { return {req.id, "Barry Allen", "The Flash"}; }
[[= grpc::rpc]]
std::optional<Hero> find_hero(HeroId req) {
  if (req.id == 1) return flash();
  return std::nullopt;
}
[[= grpc::rpc]]
Proto<Hero> get_proto(HeroId req) { return {{req.id, "Barry Allen", "The Flash"}}; }
[[= grpc::rpc]]
Json<Hero> get_json(HeroId req) { return {{req.id, "Barry Allen", "The Flash"}}; }
}  // namespace heroes

int main() {
  auto app = build();
  app.mount("/", reflect_routes<^^web>()).mount("league", reflect_routes<^^heroes>(), Mode::Grpc);
  LocalClient client(app);

  section("HTTP: plain values are JSON");
  auto p = client.get("/plain").dispatch();
  CHECK(p.status == 200 && p.headers.get("content-type") == "application/json");
  CHECK(p.body == R"({"id":1,"name":"Barry Allen","alias":"The Flash"})");
  auto m = client.get("/maybe/1").dispatch();
  CHECK(m.status == 200 && contains(m.body, "Barry Allen"));
  CHECK(client.get("/maybe/2").dispatch().status == 404);
  auto l = client.get("/list").dispatch();
  CHECK(l.status == 200 && l.body.starts_with("[{") && contains(l.body, "Dick Grayson"));
  auto c = client.post("/created").dispatch();
  CHECK(c.status == 201 && c.headers.get("location") == "/heroes/3" && contains(c.body, "Tim Drake"));
  CHECK(client.get("/expected").dispatch().body == p.body);

  section("HTTP: explicit wrappers win, strings unchanged");
  CHECK(client.get("/explicit-json").dispatch().body == p.body);
  auto pr = client.get("/explicit-proto").dispatch();
  CHECK(pr.headers.get("content-type") == "application/x-protobuf");
  CHECK(proto::decode<Hero>(pr.body).value_or(Hero{}).name == "Barry Allen");
  auto t = client.get("/text").dispatch();
  CHECK(t.body == "plain text" && contains(*t.headers.get("content-type"), "text/plain"));

  section("gRPC: plain values are protobuf");
  auto g = client.grpc("/league.Heroes/GetHero", proto::encode(HeroId{7})).dispatch();
  auto gh = grpc::read_reply<Hero>(g);
  CHECK(gh && gh->id == 7 && gh->name == "Barry Allen");
  CHECK(g.headers.get("content-type") == "application/grpc");
  CHECK(grpc::read_reply<Hero>(client.grpc("/league.Heroes/FindHero", proto::encode(HeroId{1})).dispatch()));
  auto nf = client.grpc("/league.Heroes/FindHero", proto::encode(HeroId{2})).dispatch();
  CHECK(grpc::status_of(nf).code == grpc::Code::NotFound);
  auto gp = grpc::read_reply<Hero>(client.grpc("/league.Heroes/GetProto", proto::encode(HeroId{1})).dispatch());
  CHECK(gp && gp->alias == "The Flash");
  auto gj = client.grpc("/league.Heroes/GetJson", proto::encode(HeroId{1})).dispatch();
  CHECK(gj.headers.get("content-type") == "application/grpc+json");
  CHECK(contains(gj.body, R"("name":"Barry Allen")"));

  section("OpenAPI and .proto describe them");
  auto doc = openapi_document(app);
  CHECK(contains(doc, R"("/plain")") && contains(doc, R"("#/components/schemas/Hero")"));
  CHECK(contains(doc, R"("application/x-protobuf")"));
  CHECK(contains(doc, R"("201")"));
  auto pf = grpc::proto_file<^^heroes>("league");
  CHECK(contains(pf, "rpc GetHero (HeroId) returns (Hero)") && contains(pf, "rpc GetJson (HeroId) returns (Hero)"));
  if (g_failures) std::fprintf(stderr, "%s\n%s\n", doc.c_str(), pf.c_str());

  std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures ? 1 : 0;
}
