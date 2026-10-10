// OpenAPI from reflection: the document for an API that uses every kind of
// parameter, body and return type, checked piece by piece; and the OpenApi
// fairing serving it (and the docs page) in-process.
//
//   crocket_openapi_test --print   writes the document to stdout

#include <crocket/crocket.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <variant>
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

namespace shop {

enum class Status { pending, shipped [[= json::rename("SHIPPED")]] };
enum class [[= json::rename_all(json::camel_case)]] Channel { web_store, phone_order };

struct Line {
  [[= json::min_len(1)]] std::string sku;
  [[= json::min(1), = json::max(99)]] int qty = 1;
  double price = 0;
};
struct Card { std::string last4; };
struct [[= json::rename("bank")]] Transfer { std::string iban; };
struct Category;
struct Category {
  std::string name;
  std::vector<Category> children = {};  // recursive
};
struct [[= json::rename_all(json::camel_case)]] Order {
  [[= json::from_path]] std::optional<std::uint64_t> id;
  [[= json::email]] std::string customer_email;
  [[= json::min_len(1), = json::max_len(50)]] std::vector<Line> lines;
  Status status = Status::pending;
  Channel channel = Channel::web_store;
  [[= json::as_string]] std::uint64_t ledger_ref = 0;
  [[= json::tag("method")]] std::variant<Card, Transfer> payment;
  std::variant<std::int64_t, std::string> reference = {};
  std::optional<std::chrono::sys_seconds> placed_at = {};
  [[= json::unix_millis]] std::chrono::sys_time<std::chrono::milliseconds> updated = {};
  std::chrono::year_month_day ship_by = {};
  std::chrono::milliseconds timeout{};
  std::map<std::string, std::string> notes = {};
  [[= json::pattern("^[A-Z]{3}$")]] std::optional<std::string> currency;
  std::optional<Category> category = {};
  json::Value extra = {};
};
struct [[= json::deny_unknown_fields]] Patch {
  std::optional<std::string> note;
};
struct Page {
  int page = 1;
  std::optional<std::string> q;
  bool all = false;
};

[[= http::get("/orders")]]
auto list_orders(Query<Page> p) -> Json<std::vector<Order>> { return Json{std::vector<Order>{}}; }

[[= http::get("/orders/{id}")]]
auto get_order(std::uint64_t id) -> std::optional<Json<Order>> { return std::nullopt; }

[[= http::post("/orders")]]
auto create_order(Json<Order> o, Auth who) -> Created<Order> { return {o.value}; }

[[= http::put("/orders/{id}")]]
auto replace_order(std::uint64_t id, Json<Order> o) -> Result<Json<Order>> { return Json{o.value}; }

[[= http::patch("/orders/{id}")]]
auto patchOrder(std::uint64_t id, Json<Patch> p, Header<"x-tenant"> tenant,
                Header<"x-page-size", std::optional<int>> size) -> NoContent { return {}; }

[[= http::del("/orders/{id}")]]
auto delete_order(std::uint64_t id) -> Result<void> { return {}; }

[[= http::get("/orders/{id}/label")]]
auto label(std::uint64_t id) -> Cacheable<std::string> { return {"label"}; }

[[= http::get("/orders/{id}/events")]]
auto events(std::uint64_t id) -> Task<Json<std::vector<std::string>>> { co_return Json{std::vector<std::string>{}}; }

}  // namespace shop

namespace billing {
struct Order { int total = 0; };  // same name as shop::Order
[[= http::get("/invoices/{n}")]]
auto invoice(int n) -> Json<Order> { return Json{Order{}}; }
}  // namespace billing

namespace greeter {
struct Hi { std::string name; };
struct HiReply { std::string message; };
[[= grpc::rpc]]
auto say_hello(Hi h) -> HiReply { return {h.name}; }
}  // namespace greeter

static const json::Value* at(const json::Value& v, std::initializer_list<std::string_view> path) {
  const json::Value* cur = &v;
  for (auto k : path) {
    if (!cur) return nullptr;
    if (cur->is_array()) {
      std::size_t i = std::stoul(std::string(k));
      cur = i < cur->as_array().size() ? &cur->as_array()[i] : nullptr;
    } else {
      cur = cur->find(k);
    }
  }
  return cur;
}
static std::string text(const json::Value* v) {
  if (!v) return "<missing>";
  std::string out;
  json::dump(*v, out);
  return out;
}

int main(int argc, char** argv) {
  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  Crocket app{cfg};
  app.manage(Authenticator{[](std::string_view) -> std::expected<Auth, ApiError> { return Auth{"x", {}}; }})
      .mount("/api", reflect_routes<^^shop>())
      .mount("/billing", reflect_routes<^^billing>())
      .mount("demo", reflect_routes<^^greeter>(), Mode::Grpc)
      .attach(OpenApi{{.title = "Shop", .version = "2.1.0", .description = "Orders and invoices", .docs = "/docs"}});

  std::string doc_text = openapi_document(app, {.title = "Shop", .version = "2.1.0", .description = "Orders and invoices"});
  if (argc > 1 && std::string_view(argv[1]) == "--print") {
    std::printf("%s\n", doc_text.c_str());
    return 0;
  }
  auto parsed = json::parse(doc_text);
  CHECK(parsed.has_value());
  if (!parsed) return 1;
  const json::Value& doc = *parsed;
  auto schemas = at(doc, {"components", "schemas"});

  section("document");
  CHECK(text(at(doc, {"openapi"})) == R"("3.1.0")");
  CHECK(text(at(doc, {"info"})) == R"({"title":"Shop","version":"2.1.0","description":"Orders and invoices"})");
  CHECK(at(doc, {"paths", "/api/orders/{id}", "get"}));
  CHECK(at(doc, {"paths", "/healthz", "get"}));
  CHECK(!at(doc, {"paths", "/demo.Greeter/SayHello"}));  // gRPC is not OpenAPI's

  section("operations");
  auto get_order = at(doc, {"paths", "/api/orders/{id}", "get"});
  CHECK(text(at(*get_order, {"operationId"})) == R"("shop.get_order")");
  CHECK(text(at(*get_order, {"summary"})) == R"("Get order")");
  CHECK(text(at(*get_order, {"tags"})) == R"(["shop"])");
  CHECK(text(at(*get_order, {"parameters"})) ==
        R"([{"name":"id","in":"path","required":true,"schema":{"type":"integer","format":"uint64","minimum":0}}])");
  CHECK(text(at(*get_order, {"responses", "200", "content", "application/json", "schema"})) ==
        R"({"$ref":"#/components/schemas/Order"})");
  CHECK(at(*get_order, {"responses", "404"}));  // std::optional
  CHECK(text(at(*get_order, {"responses", "default", "content", "application/problem+json", "schema"})) ==
        R"({"$ref":"#/components/schemas/Problem"})");
  CHECK(text(at(doc, {"paths", "/api/orders/{id}", "patch", "summary"})) == R"("Patch order")");

  auto list = at(doc, {"paths", "/api/orders", "get"});
  CHECK(text(at(*list, {"parameters"})) ==
        R"([{"name":"page","in":"query","required":false,"schema":{"type":"integer","format":"int32"}},)"
        R"({"name":"q","in":"query","required":false,"schema":{"type":"string"}},)"
        R"({"name":"all","in":"query","required":false,"schema":{"type":"boolean"}}])");
  CHECK(text(at(*list, {"responses", "200", "content", "application/json", "schema"})) ==
        R"({"type":"array","items":{"$ref":"#/components/schemas/Order"}})");
  CHECK(at(*list, {"responses", "422"}));

  auto create = at(doc, {"paths", "/api/orders", "post"});
  CHECK(text(at(*create, {"requestBody", "required"})) == "true");
  CHECK(text(at(*create, {"requestBody", "content", "application/json", "schema"})) ==
        R"({"$ref":"#/components/schemas/Order"})");
  CHECK(at(*create, {"responses", "201"}) && !at(*create, {"responses", "200"}));  // Created<T>
  CHECK(at(*create, {"responses", "401"}) && at(*create, {"responses", "415"}) && at(*create, {"responses", "422"}));
  CHECK(text(at(*create, {"security"})) == R"([{"bearer":[]}])");
  CHECK(text(at(doc, {"components", "securitySchemes", "bearer"})) == R"({"type":"http","scheme":"bearer"})");

  auto patch = at(doc, {"paths", "/api/orders/{id}", "patch"});
  CHECK(text(at(*patch, {"parameters", "1"})) ==
        R"({"name":"x-tenant","in":"header","required":true,"schema":{"type":"string"}})");
  CHECK(text(at(*patch, {"parameters", "2"})) ==
        R"({"name":"x-page-size","in":"header","required":false,"schema":{"type":"integer","format":"int32"}})");
  CHECK(at(*patch, {"responses", "204"}) && !at(*patch, {"responses", "204", "content"}));
  CHECK(at(doc, {"paths", "/api/orders/{id}", "delete", "responses", "204"}));  // Result<void>
  auto label = at(doc, {"paths", "/api/orders/{id}/label", "get", "responses"});
  CHECK(text(at(*label, {"200", "content", "text/plain", "schema"})) == R"({"type":"string"})");
  CHECK(at(*label, {"304"}));  // Cacheable
  CHECK(text(at(doc, {"paths", "/api/orders/{id}/events", "get", "responses", "200", "content", "application/json", "schema"})) ==
        R"({"type":"array","items":{"type":"string"}})");  // Task<T>
  // Responses in order: successes, then errors by code, then default.
  std::string order;
  for (auto& [code, r] : at(*create, {"responses"})->as_object()) order += code + " ";
  CHECK(order == "201 401 415 422 default ");

  section("schemas");
  auto o = at(*schemas, {"Order", "properties"});
  CHECK(text(at(*schemas, {"Order", "required"})) == R"(["customerEmail","lines","payment"])");
  CHECK(text(at(*o, {"id"})) == R"({"type":["integer","null"],"format":"uint64","minimum":0,"readOnly":true})");
  CHECK(text(at(*o, {"customerEmail"})) == R"({"type":"string","format":"email"})");
  CHECK(text(at(*o, {"lines"})) ==
        R"({"type":"array","items":{"$ref":"#/components/schemas/Line"},"minItems":1,"maxItems":50})");
  CHECK(text(at(*o, {"status"})) == R"({"type":"string","enum":["pending","SHIPPED"]})");
  CHECK(text(at(*o, {"channel"})) == R"({"type":"string","enum":["webStore","phoneOrder"]})");
  CHECK(text(at(*o, {"ledgerRef"})) == R"({"type":"string","format":"uint64","pattern":"^[0-9]+$"})");
  CHECK(text(at(*o, {"payment", "discriminator"})) ==
        R"({"propertyName":"method","mapping":{"Card":"#/components/schemas/Card","bank":"#/components/schemas/Transfer"}})");
  CHECK(text(at(*o, {"payment", "oneOf", "1", "allOf", "1", "properties"})) == R"({"method":{"const":"bank"}})");
  CHECK(text(at(*o, {"reference"})) ==
        R"({"anyOf":[{"type":"integer","format":"int64"},{"type":"string"}]})");
  CHECK(text(at(*o, {"placedAt"})) == R"({"type":["string","null"],"format":"date-time"})");
  CHECK(text(at(*o, {"updated", "type"})) == R"("integer")");
  CHECK(text(at(*o, {"shipBy"})) == R"({"type":"string","format":"date"})");
  CHECK(text(at(*o, {"timeout"})) == R"({"type":"integer","description":"A duration, in milliseconds"})");
  CHECK(text(at(*o, {"notes"})) == R"({"type":"object","additionalProperties":{"type":"string"}})");
  CHECK(text(at(*o, {"currency"})) == R"({"type":["string","null"],"pattern":"^[A-Z]{3}$"})");
  CHECK(text(at(*o, {"category"})) ==
        R"({"anyOf":[{"$ref":"#/components/schemas/Category"},{"type":"null"}]})");
  CHECK(text(at(*o, {"extra"})) == "{}");
  CHECK(text(at(*schemas, {"Line", "properties", "qty"})) ==
        R"({"type":"integer","format":"int32","minimum":1,"maximum":99})");
  CHECK(text(at(*schemas, {"Line", "properties", "sku"})) == R"({"type":"string","minLength":1})");
  CHECK(text(at(*schemas, {"Category", "properties", "children", "items"})) ==
        R"({"$ref":"#/components/schemas/Category"})");  // recursion by reference
  CHECK(text(at(*schemas, {"Patch", "additionalProperties"})) == "false");
  // Two structs named Order: the second gets its qualified name.
  CHECK(text(at(doc, {"paths", "/billing/invoices/{n}", "get", "responses", "200", "content", "application/json", "schema"})) ==
        R"({"$ref":"#/components/schemas/billing.Order"})");
  CHECK(at(*schemas, {"billing.Order"}));
  CHECK(at(*schemas, {"Problem", "properties", "code"}));
  // Every $ref points at a schema that exists.
  bool refs_ok = true;
  constexpr std::string_view needle = R"("$ref":"#/components/schemas/)";
  std::size_t at_ref = 0;
  while ((at_ref = doc_text.find(needle, at_ref)) != std::string::npos) {
    at_ref += needle.size();
    auto name = doc_text.substr(at_ref, doc_text.find('"', at_ref) - at_ref);
    if (!schemas->find(name)) {
      refs_ok = false;
      std::fprintf(stderr, "  dangling $ref %s\n", name.c_str());
    }
  }
  CHECK(refs_ok);

  section("served by the OpenApi fairing");
  LocalClient client(app);
  auto served = client.get("/openapi.json").dispatch();
  CHECK(served.status == 200);
  CHECK(served.headers.get("content-type") == std::optional<std::string_view>("application/json"));
  CHECK(served.body == doc_text);
  auto page = client.get("/docs").dispatch();
  CHECK(page.status == 200);
  CHECK(page.body.find(R"(data-spec="/openapi.json")") != std::string::npos);
  CHECK(page.headers.get("content-security-policy")->find("script-src 'self' https://cdn.jsdelivr.net") !=
        std::string_view::npos);
  auto script = client.get("/docs/init.js").dispatch();
  CHECK(script.status == 200 && script.body.find("SwaggerUIBundle") != std::string::npos);
  CHECK(client.get("/openapi.yaml").dispatch().status == 404);

  section("a bad path fails ignite");
  {
    Crocket bad{cfg};
    bad.attach(OpenApi{{.path = "openapi.json"}});
    auto r = bad.ignite();
    CHECK(!r && r.error().message().find("must start with '/'") != std::string::npos);
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
