// Acceptance tests from REQUIREMENTS.md, run in-process through LocalClient
// (same pipeline as the network engine, no sockets). Items 5 and 7 live in
// tests/compile_fail and tests/tls_h2.sh.

#include <crocket/crocket.hpp>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace crocket;

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

// ---- application under test ------------------------------------------------------

struct NewUser {
  std::string email;
  std::optional<std::string> name;
};
struct User {
  std::uint64_t id;
  std::string email;
  std::string owner;
};

/// Stand-in for a database handle. Thread-safe by construction.
struct Db {
  std::atomic<std::uint64_t> next_id{1};
  bool healthy = true;
  Db() = default;
  Db(Db&& o) noexcept : next_id(o.next_id.load()), healthy(o.healthy) {}
  [[nodiscard]] bool ready() const { return healthy; }
};

std::atomic<int> g_hello_calls{0};
std::atomic<int> g_create_calls{0};

struct Target {
  std::string to;
};

namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  ++g_hello_calls;
  return std::format("Hello, {} year old named {}!", age, name);
}

[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth, State<Db> db) -> Result<Created<User>, ApiError> {
  ++g_create_calls;
  if (body->email.find('@') == std::string::npos)
    return std::unexpected(ApiError::unprocessable("user.email", "email must contain '@'"));
  User u{db->next_id++, body->email, auth.subject};
  return Created<User>{u, "/users/" + std::to_string(u.id)};
}

[[= http::get("/me")]]
auto me(Auth auth) -> Json<User> {
  return Json<User>{{0, auth.subject + "@example.com", auth.subject}};
}

auto helper(int x) -> int { return x + 1; }  // no annotation: not a route

// Ranked forwarding: numeric ids first, then slugs.
[[= http::get("/items/{id}", {.rank = 1})]]
auto item_by_id(std::uint32_t id) -> std::string { return "id:" + std::to_string(id); }

[[= http::get("/items/{slug}", {.rank = 2})]]
auto item_by_slug(std::string slug) -> std::string { return "slug:" + slug; }

// Literal segment beats capture regardless of declaration order.
[[= http::get("/items/special")]]
auto item_special() -> std::string { return "special"; }

[[= http::get("/maybe/{n}")]]
auto maybe(int n) -> std::optional<Json<int>> {
  if (n < 0) return std::nullopt;
  return Json<int>{n * 2};
}

[[= http::get("/redirect")]]  // a Location built from untrusted input
auto redirect(Query<Target> q, Response& rs) -> void {
  rs.status = 302;
  rs.headers.set("location", q->to);
}

[[= http::get("/boom")]]
auto boom() -> std::string { throw std::runtime_error("secret database password in what()"); }

[[= http::del("/users/{id}")]]
auto remove(std::uint64_t id, Auth auth) -> Result<void> {
  if (!auth.has_scope("admin")) return std::unexpected(ApiError::forbidden("auth.scope", "admin scope required"));
  (void)id;
  return {};
}

struct Page {
  int page = 1;
  std::optional<std::string> q;
};
[[= http::get("/search")]]
auto search(Query<Page> p) -> std::string { return std::format("page={} q={}", p->page, p->q.value_or("-")); }

[[= http::get("/async/{n}")]]
auto async_double(int n) -> Task<std::string> {
  auto inner = [](int x) -> Task<int> { co_return x * 2; };
  int v = co_await inner(n);
  co_return std::to_string(v);
}

[[= http::get("/raw")]]
auto raw(const Request& rq, Response& rs) -> void {
  rs.status = 299;
  rs.body = std::string(rq.handler);
}

[[= http::get("/deadline")]]
auto remaining(Deadline d, RequestId id) -> std::string {
  return std::format("{} {}", d.expired() ? "expired" : "ok", id.value.size());
}

}  // namespace api

namespace other {
[[= http::get("/needs-db")]]
auto needs_db(State<Db> db) -> std::string { return std::to_string(db->next_id.load()); }
}  // namespace other

// Controller: non-static members run on the managed instance.
class Greeter {
 public:
  explicit Greeter(std::string greeting) : greeting_(std::move(greeting)) {}

  [[= http::get("/greet/{who}")]]
  auto greet(std::string_view who) const -> std::string { return greeting_ + ", " + std::string(who); }

  [[= http::get("/version")]]
  static auto version() -> std::string { return "1"; }

 private:
  std::string greeting_;
};

namespace dup {
[[= http::get("/x/{a}")]]
auto a(int a) -> std::string { return std::to_string(a); }
[[= http::get("/x/{b}")]]
auto b(int b) -> std::string { return std::to_string(b); }
}  // namespace dup

// ---- helpers -------------------------------------------------------------------

Authenticator test_auth() {
  return Authenticator{[](std::string_view token) -> std::expected<Auth, ApiError> {
    if (token == "alice") return Auth{"alice", {}};
    if (token == "root") return Auth{"root", {"admin"}};
    if (token == "old") return std::unexpected(ApiError::unauthorized("auth.expired", "token expired"));
    return std::unexpected(ApiError::unauthorized("auth.invalid", "unknown token"));
  }};
}

struct LogCapture {
  std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
  std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();
  Logger logger() {
    return Logger{[mu = mu, lines = lines](std::string_view l) {
      std::lock_guard lk(*mu);
      lines->emplace_back(l);
    }};
  }
  std::string last() const {
    std::lock_guard lk(*mu);
    return lines->empty() ? "" : lines->back();
  }
};

// ---- [[= json::from_path]]: one struct for create and update ---------------------
struct Widget {
  [[= json::from_path]] std::optional<std::uint64_t> id;
  std::string name;
  int size = 1;
};
struct Tag {
  [[= json::from_path]] std::optional<std::string> slug;
  std::string label;
};

namespace resources {
[[= http::post("/widgets")]]
auto create(Json<Widget> w) -> Created<Json<Widget>> {
  w->id = 100;  // server-assigned
  return {Json<Widget>{*w}, "/widgets/100"};
}
[[= http::put("/widgets/{id}")]]
auto replace(std::uint64_t id, Json<Widget> w) -> Json<Widget> {
  (void)id;
  return Json<Widget>{*w};
}
[[= http::patch("/widgets/{id}")]]
auto patch(std::uint64_t id, Json<Widget> w) -> Json<Widget> {
  (void)id;
  return Json<Widget>{*w};
}
[[= http::put("/by-name/{id}")]]  // capture typed as text; the body field needs a number
auto by_name(std::string_view id, Json<Widget> w) -> Json<Widget> {
  (void)id;
  return Json<Widget>{*w};
}
[[= http::put("/tags/{slug}")]]
auto put_tag(std::string_view slug, Json<Tag> t) -> Json<Tag> {
  (void)slug;
  return Json<Tag>{*t};
}
[[= http::post("/widgets/bulk")]]  // arrays: only a top-level struct is checked
auto bulk(Json<std::vector<Widget>> ws) -> std::string { return std::to_string(ws->size()); }
}  // namespace resources

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

Crocket make_app(LogCapture& log, Metrics* metrics_out = nullptr, Config cfg = Config{.debug_routes = true}) {
  Crocket app{std::move(cfg)};
  app.manage(Db{}).manage(test_auth()).manage(Greeter{"Hi"}).attach(log.logger());
  if (metrics_out) app.attach(*metrics_out);
  app.attach(Cors::allow_origins({"https://ok.example"}))
      .mount("/", reflect_routes<^^api>())
      .mount("/c", reflect_routes<^^Greeter>());
  return app;
}

// ---- tests -----------------------------------------------------------------------

int main() {
  LogCapture log;
  Metrics metrics;
  Crocket app = make_app(log, &metrics);
  LocalClient client(app);

  section("1. hello(name, uint8_t age)");
  {
    auto r = client.get("/hello/Rocketeer/42").dispatch();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string("Hello, 42 year old named Rocketeer!"));
    CHECK(contains(*r.headers.get("content-type"), "text/plain"));
    int before = g_hello_calls;
    for (auto bad : {"/hello/Bob/old", "/hello/Bob/256", "/hello/Bob/-1", "/hello/Bob/4x"}) {
      auto e = client.get(bad).dispatch();
      CHECK_EQ(e.status, 404);
      CHECK_EQ(e.error_code, std::string_view("path.invalid"));
    }
    CHECK_EQ(g_hello_calls.load(), before);  // function did not run
    auto pct = client.get("/hello/Ada%20L/36").dispatch();
    CHECK_EQ(pct.body, std::string("Hello, 36 year old named Ada L!"));
  }

  section("2. create(Json<NewUser>, Auth, State<Db>)");
  {
    int before = g_create_calls;
    auto missing_body = client.post("/users").bearer("alice").dispatch();
    CHECK_EQ(missing_body.status, 422);
    CHECK_EQ(missing_body.error_code, std::string_view("json.invalid"));
    auto bad_json = client.post("/users").bearer("alice").json(R"({"email": )").dispatch();
    CHECK_EQ(bad_json.status, 422);
    CHECK(contains(bad_json.body, "malformed JSON"));
    auto wrong_shape = client.post("/users").bearer("alice").json(R"({"email": 7})").dispatch();
    CHECK_EQ(wrong_shape.status, 422);
    CHECK(contains(wrong_shape.body, "field 'email'"));
    auto no_auth = client.post("/users").json(R"({"email":"a@b.c"})").dispatch();
    CHECK_EQ(no_auth.status, 401);
    CHECK_EQ(no_auth.error_code, std::string_view("auth.missing"));
    CHECK(no_auth.headers.contains("www-authenticate"));
    auto expired = client.post("/users").bearer("old").json(R"({"email":"a@b.c"})").dispatch();
    CHECK_EQ(expired.status, 401);
    CHECK_EQ(expired.error_code, std::string_view("auth.expired"));
    auto wrong_ct = client.post("/users").bearer("alice").header("content-type", "text/plain").body("x").dispatch();
    CHECK_EQ(wrong_ct.status, 415);
    CHECK_EQ(g_create_calls.load(), before);  // none of the above reached the handler

    auto ok = client.post("/users").bearer("alice").json(R"({"email":"a@b.c"})").dispatch();
    CHECK_EQ(ok.status, 201);
    CHECK_EQ(ok.body, std::string(R"({"id":1,"email":"a@b.c","owner":"alice"})"));
    CHECK_EQ(*ok.headers.get("location"), std::string_view("/users/1"));
    auto domain_err = client.post("/users").bearer("alice").json(R"({"email":"nope"})").dispatch();
    CHECK_EQ(domain_err.status, 422);
    CHECK_EQ(domain_err.error_code, std::string_view("user.email"));
  }

  section("3. me(Auth): no body extractor");
  {
    auto r = client.get("/me").bearer("alice").body("ignored body").dispatch();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string(R"({"id":0,"email":"alice@example.com","owner":"alice"})"));
    CHECK_EQ(*r.headers.get("content-type"), std::string_view("application/json"));
    CHECK_EQ(client.get("/me").dispatch().status, 401);
  }

  section("4. helper is not a route");
  {
    CHECK_EQ(api::helper(1), 2);
    CHECK_EQ(client.get("/helper").dispatch().status, 404);
    bool found = false;
    for (auto& r : app.routes()) found = found || contains(r.handler, "helper");
    CHECK(!found);
  }

  section("6. missing .manage(Db) fails ignite");
  {
    Crocket bare;
    bare.mount("/", reflect_routes<^^other>());
    auto ig = bare.ignite();
    CHECK(!ig.has_value());
    if (!ig) {
      CHECK(contains(ig.error().message(), "State<Db>"));
      CHECK(contains(ig.error().message(), "other::needs_db"));
    }
    Crocket fixed;
    fixed.manage(Db{}).mount("/", reflect_routes<^^other>());
    CHECK(fixed.ignite().has_value());
    // Auth needs a managed Authenticator, checked the same way.
    Crocket no_auth;
    no_auth.manage(Db{}).manage(Greeter{"x"}).mount("/", reflect_routes<^^api>());
    auto ig2 = no_auth.ignite();
    CHECK(!ig2 && contains(ig2.error().message(), "State<crocket::Authenticator>"));
  }

  section("ignite: conflicting verb+path");
  {
    Crocket conflict;
    conflict.mount("/", reflect_routes<^^dup>());
    auto ig = conflict.ignite();
    CHECK(!ig && contains(ig.error().message(), "route conflict"));
    Crocket twice;
    twice.mount("/", reflect_routes<^^other>()).mount("/", reflect_routes<^^other>()).manage(Db{});
    CHECK(!twice.ignite());
  }

  section("routing: rank, forwarding, specificity, 405, HEAD");
  {
    CHECK_EQ(client.get("/items/7").dispatch().body, std::string("id:7"));
    CHECK_EQ(client.get("/items/widget").dispatch().body, std::string("slug:widget"));
    CHECK_EQ(client.get("/items/special").dispatch().body, std::string("special"));
    auto m = client.put("/items/7").dispatch();
    CHECK_EQ(m.status, 405);
    CHECK(contains(*m.headers.get("allow"), "GET"));
    auto h = client.head("/hello/a/1").dispatch();
    CHECK_EQ(h.status, 200);
    CHECK_EQ(client.get("/hello/a/1/extra").dispatch().status, 404);
  }

  section("responders: optional, expected<void>, async, raw, controller");
  {
    CHECK_EQ(client.get("/maybe/21").dispatch().body, std::string("42"));
    auto none = client.get("/maybe/-1").dispatch();
    CHECK_EQ(none.status, 404);
    CHECK_EQ(none.error_code, std::string_view("not_found"));
    CHECK_EQ(client.del("/users/3").bearer("alice").dispatch().status, 403);
    CHECK_EQ(client.del("/users/3").bearer("root").dispatch().status, 204);
    CHECK_EQ(client.get("/async/21").dispatch().body, std::string("42"));
    auto raw = client.get("/raw").dispatch();
    CHECK_EQ(raw.status, 299);
    CHECK_EQ(raw.body, std::string("api::raw"));
    CHECK_EQ(client.get("/c/greet/Sean").dispatch().body, std::string("Hi, Sean"));
    CHECK_EQ(client.get("/c/version").dispatch().body, std::string("1"));
    CHECK(contains(client.get("/deadline").dispatch().body, "ok 32"));
  }

  section("query extractor");
  {
    CHECK_EQ(client.get("/search?q=red+shoes&page=3").dispatch().body, std::string("page=3 q=red shoes"));
    CHECK_EQ(client.get("/search").dispatch().body, std::string("page=1 q=-"));
    auto bad = client.get("/search?page=two").dispatch();
    CHECK_EQ(bad.status, 422);
    CHECK_EQ(bad.error_code, std::string_view("query.invalid"));
  }

  section("errors: uncaught exception never leaks what()");
  {
    auto r = client.get("/boom").dispatch();
    CHECK_EQ(r.status, 500);
    CHECK_EQ(r.error_code, std::string_view("internal"));
    CHECK(!contains(r.body, "secret"));
    CHECK(contains(r.body, R"("code":"internal")"));
    CHECK(contains(log.last(), "secret database password"));  // detail goes to logs only
    CHECK(contains(log.last(), R"("route":"/boom")"));
  }

  section("request ids");
  {
    auto given = client.get("/hello/a/1").header("X-Request-Id", "trace-abc.123").dispatch();
    CHECK_EQ(*given.headers.get("x-request-id"), std::string_view("trace-abc.123"));
    auto bogus = client.get("/nope").header("X-Request-Id", "has spaces!").dispatch();
    auto id = *bogus.headers.get("x-request-id");
    CHECK_EQ(id.size(), std::size_t(32));
    CHECK(contains(bogus.body, std::string(id)));  // error body carries the id
    CHECK(contains(log.last(), std::string(id)));  // and so does the log line
  }

  section("completion log line");
  {
    (void)client.get("/hello/Ada/36").dispatch();
    auto line = log.last();
    for (auto key : {R"("method":"GET")", R"("route":"/hello/{name}/{age}")", R"("status":200)",
                     R"("code":"")", R"("duration_ms":)", R"("bytes":29)", R"("id":")"})
      CHECK(contains(line, key));
    CHECK(!contains(line, "Ada"));  // route template, not the raw path
  }

  section("health, readiness, __routes");
  {
    CHECK_EQ(client.get("/healthz").dispatch().status, 200);
    auto ready = client.get("/readyz").dispatch();
    CHECK_EQ(ready.status, 200);
    CHECK(contains(ready.body, R"("Db":"ok")"));
    app.core().state.get<Db>()->healthy = false;
    auto not_ready = client.get("/readyz").dispatch();
    CHECK_EQ(not_ready.status, 503);
    CHECK(contains(not_ready.body, R"("Db":"fail")"));
    app.core().state.get<Db>()->healthy = true;
    auto routes = client.get("/__routes").dispatch();
    CHECK(contains(routes.body, R"("handler":"api::create")"));
    CHECK(contains(routes.body, R"({"name":"body","source":"json")"));
    CHECK(contains(routes.body, R"("rank":2)"));
    Crocket prod;
    LocalClient pc(prod);
    CHECK_EQ(pc.get("/__routes").dispatch().status, 404);  // debug-only
  }

  section("CORS fairing");
  {
    auto pre = client.options("/users")
                   .header("origin", "https://ok.example")
                   .header("access-control-request-method", "POST")
                   .header("access-control-request-headers", "Content-Type, Authorization")
                   .dispatch();
    CHECK_EQ(pre.status, 204);
    CHECK_EQ(*pre.headers.get("access-control-allow-origin"), std::string_view("https://ok.example"));
    auto denied = client.options("/users")
                      .header("origin", "https://evil.example")
                      .header("access-control-request-method", "POST")
                      .dispatch();
    CHECK_EQ(denied.status, 403);
    CHECK_EQ(denied.error_code, std::string_view("cors.denied"));
    auto simple = client.get("/hello/a/1").header("origin", "https://ok.example").dispatch();
    CHECK(simple.headers.contains("access-control-allow-origin"));
    auto foreign = client.get("/hello/a/1").header("origin", "https://evil.example").dispatch();
    CHECK(!foreign.headers.contains("access-control-allow-origin"));

    Crocket deny_app;
    deny_app.attach(Cors::deny()).mount("/", reflect_routes<^^other>()).manage(Db{});
    LocalClient dc(deny_app);
    auto d = dc.options("/needs-db").header("origin", "https://ok.example").header("access-control-request-method", "GET").dispatch();
    CHECK_EQ(d.status, 403);
  }

  section("metrics labelled by route template");
  {
    auto m = client.get("/metrics").dispatch();
    CHECK_EQ(m.status, 200);
    CHECK(contains(m.body, R"(crocket_http_requests_total{method="GET",route="/hello/{name}/{age}",status="200"})"));
    CHECK(contains(m.body, R"(crocket_extractor_failures_total{route="/users",kind="json"})"));
    CHECK(contains(m.body, R"(crocket_extractor_failures_total{route="/users",kind="auth"})"));
    CHECK(contains(m.body, R"(crocket_extractor_failures_total{route="/hello/{name}/{age}",kind="path"})"));
    CHECK(contains(m.body, "crocket_http_requests_in_flight 1"));  // the scrape itself
    CHECK(!contains(m.body, "Rocketeer"));
  }

  section("pool: 503 db.busy after the deadline");
  {
    Pool<int> pool(1, [] { return 7; }, {.max_wait = std::chrono::milliseconds(50)});
    auto first = pool.checkout(Deadline{Clock::now() + std::chrono::seconds(1), {}});
    CHECK(first.has_value());
    auto second = pool.checkout(Deadline{Clock::now() + std::chrono::seconds(1), {}});
    CHECK(!second.has_value());
    if (!second) {
      CHECK_EQ(second.error().status, 503);
      CHECK_EQ(second.error().code, std::string_view("db.busy"));
    }
    CHECK(!pool.ready());
    { auto drop = std::move(*first); }
    CHECK(pool.ready());
    std::stop_source cancel;
    auto hold = pool.checkout(Deadline{Clock::now() + std::chrono::seconds(1), {}});
    std::jthread canceller([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      cancel.request_stop();
    });
    auto t0 = Clock::now();
    auto cancelled = pool.checkout(Deadline{Clock::now() + std::chrono::seconds(10), cancel.get_token()});
    CHECK(!cancelled.has_value());
    CHECK(Clock::now() - t0 < std::chrono::seconds(1));
  }

  section("pool: reassigning a lease returns its resource");
  {
    Pool<int> pool(2, [] { return 0; });
    Deadline d{Clock::now() + std::chrono::seconds(1), {}};
    {
      auto a = pool.checkout(d);
      auto b = pool.checkout(d);
      CHECK_EQ(pool.available(), std::size_t(0));
      *a = std::move(*b);  // a's resource goes back now; b's goes back when a ends
      CHECK_EQ(pool.available(), std::size_t(1));
      *a = std::move(*a);  // self-move keeps the resource
      CHECK_EQ(pool.available(), std::size_t(1));
    }
    CHECK_EQ(pool.available(), std::size_t(2));
  }

  section("response headers: no CR/LF/NUL on the wire");
  {
    static_assert(http::valid_header_name("x-request-id") && !http::valid_header_name("bad name") &&
                  !http::valid_header_name("") && !http::valid_header_name("a:b"));
    static_assert(http::valid_header_value("/items/1") && !http::valid_header_value("a\r\nb") &&
                  !http::valid_header_value("a\nb") && !http::valid_header_value(std::string_view("a\0b", 3)));
    auto ok = client.get("/redirect?to=/items/1").dispatch();
    CHECK_EQ(ok.status, 302);
    CHECK_EQ(*ok.headers.get("location"), std::string_view("/items/1"));
    auto evil = client.get("/redirect?to=/x%0D%0Aset-cookie:%20pwned=1").dispatch();
    CHECK_EQ(evil.status, 500);
    CHECK_EQ(evil.error_code, std::string_view("internal"));
    CHECK(!evil.headers.contains("location"));
    CHECK(!contains(evil.body, "pwned"));
    CHECK(contains(log.last(), "response header 'location' contains a character not allowed"));
  }

  section("json::from_path: one struct for POST and PUT");
  {
    Crocket rapp;
    rapp.mount("/v1", reflect_routes<^^resources>());
    LocalClient rc(rapp);
    // POST: no {id} capture, so the server owns id.
    auto created = rc.post("/v1/widgets").json(R"({"name":"gear"})").dispatch();
    CHECK_EQ(created.status, 201);
    CHECK_EQ(created.body, std::string(R"({"id":100,"name":"gear","size":1})"));
    auto null_id = rc.post("/v1/widgets").json(R"({"id":null,"name":"gear"})").dispatch();
    CHECK_EQ(null_id.status, 201);
    auto sent_id = rc.post("/v1/widgets").json(R"({"id":7,"name":"gear"})").dispatch();
    CHECK_EQ(sent_id.status, 422);
    CHECK_EQ(sent_id.error_code, std::string_view("json.read_only"));
    CHECK(contains(sent_id.body, "field 'id' is assigned by the server"));
    // PUT: id comes from the URL (under a mount prefix).
    auto put = rc.put("/v1/widgets/42").json(R"({"name":"cog","size":3})").dispatch();
    CHECK_EQ(put.status, 200);
    CHECK_EQ(put.body, std::string(R"({"id":42,"name":"cog","size":3})"));
    auto echo = rc.put("/v1/widgets/42").json(R"({"id":42,"name":"cog"})").dispatch();
    CHECK_EQ(echo.status, 200);
    auto mismatch = rc.put("/v1/widgets/42").json(R"({"id":41,"name":"cog"})").dispatch();
    CHECK_EQ(mismatch.status, 422);
    CHECK_EQ(mismatch.error_code, std::string_view("json.path_mismatch"));
    CHECK(contains(mismatch.body, "does not match {id} in the URL"));
    auto patched = rc.patch("/v1/widgets/9").json(R"({"name":"x"})").dispatch();
    CHECK(contains(patched.body, R"("id":9)"));
    // The capture parses for the handler (string_view) but not for the field (uint64).
    auto wrong_type = rc.put("/v1/by-name/abc").json(R"({"name":"x"})").dispatch();
    CHECK_EQ(wrong_type.status, 404);
    CHECK_EQ(wrong_type.error_code, std::string_view("path.invalid"));
    CHECK(!contains(wrong_type.body, "is not a valid value"));  // detail stays in the logs
    // Non-numeric keys work too.
    auto tag = rc.put("/v1/tags/cpp26").json(R"({"label":"C++26"})").dispatch();
    CHECK_EQ(tag.body, std::string(R"({"slug":"cpp26","label":"C++26"})"));
    auto tag_bad = rc.put("/v1/tags/cpp26").json(R"({"slug":"rust","label":"x"})").dispatch();
    CHECK_EQ(tag_bad.error_code, std::string_view("json.path_mismatch"));
    // Nested/array elements are not checked.
    auto bulk = rc.post("/v1/widgets/bulk").json(R"([{"id":1,"name":"a"},{"name":"b"}])").dispatch();
    CHECK_EQ(bulk.status, 200);
    CHECK_EQ(bulk.body, std::string("2"));
    // Request::capture looks captures up by name in the matched template.
    Request probe;
    probe.route_template = "/v1/users/{uid}/posts/{pid}";
    probe.captures = {"7", "99"};
    CHECK_EQ(probe.capture("pid").value_or("?"), std::string_view("99"));
    CHECK_EQ(probe.capture("uid").value_or("?"), std::string_view("7"));
    CHECK(!probe.capture("id").has_value());
  }

  section("dev profile: details in error bodies only in dev");
  {
    // Release (the app above): the detail is logged, never sent.
    for (auto path : {"/boom", "/hello/Ada/400"}) {
      auto r = client.get(path).dispatch();
      CHECK(r.status >= 400);
      CHECK(!contains(r.body, R"("detail")"));
      CHECK(contains(log.last(), R"("detail":)"));
    }
    CHECK(Config{}.profile == Profile::Release);

    LogCapture dev_log;
    Crocket dev_app = make_app(dev_log, nullptr, Config::dev());
    LocalClient dev(dev_app);
    auto boom = dev.get("/boom").dispatch();
    CHECK_EQ(boom.status, 500);
    CHECK(contains(boom.body, R"x("detail":"uncaught exception in api::boom: secret database password in what()")x"));
    auto fwd = dev.get("/hello/Ada/400").dispatch();
    CHECK(contains(fwd.body, R"("detail":"api::hello: capture '{age}' did not parse")"));

    // Readable log lines, no JSON, no colour for a custom sink.
    auto line = dev_log.last();
    CHECK(contains(line, " GET /hello/Ada/400 404 "));
    CHECK(contains(line, "api::hello path.invalid: api::hello: capture '{age}' did not parse ["));
    CHECK(contains(line, *fwd.headers.get("x-request-id")));
    CHECK(!contains(line, "{\"ts\""));
    CHECK(!contains(line, "\x1b["));
    CHECK_EQ(dev.get("/__routes").dispatch().status, 200);

    auto d = Config::dev();
    CHECK(d.debug_routes);
    CHECK_EQ(d.request_timeout, std::chrono::milliseconds(std::chrono::hours(1)));
    CHECK_EQ(d.drain_timeout, std::chrono::milliseconds(1000));
  }

  section("dev profile: CROCKET_PROFILE");
  {
    ::unsetenv("CROCKET_PROFILE");
    CHECK(Config::from_env().profile == Profile::Release);
    ::setenv("CROCKET_PROFILE", "release", 1);
    CHECK(Config::from_env().profile == Profile::Release);
    ::setenv("CROCKET_PROFILE", "dev", 1);
    CHECK(Config::from_env().profile == Profile::Dev);
    ::setenv("CROCKET_PROFILE", "prod", 1);
    std::string err;
    try {
      (void)Config::from_env();
    } catch (const std::invalid_argument& e) {
      err = e.what();
    }
    CHECK_EQ(err, std::string("CROCKET_PROFILE=prod: unknown profile (use dev or release)"));
    ::unsetenv("CROCKET_PROFILE");
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
