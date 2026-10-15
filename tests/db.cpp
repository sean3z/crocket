// crocket::db against a real Postgres. Set CROCKET_TEST_DATABASE_URL
// (postgresql://user:pass@localhost/crocket_test); without it the test is skipped.

#include <crocket/crocket.hpp>
#include <crocket/db.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>

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
  std::optional<std::string> mentor;
};
struct HeroTeam {
  std::int64_t id;
  std::string name;
};
struct Sidekick {  // stored in a table of another name
  std::int64_t id;
  std::string name;
  int power;
};
template <>
constexpr auto crocket::db::table_name<Sidekick> = "justice_league";

// ---- names, while compiling ----------------------------------------------------
static_assert(std::string_view(db::table_name<Hero>) == "heroes");
static_assert(std::string_view(db::table_name<HeroTeam>) == "hero_teams");
static_assert(std::string_view(db::table_name<Sidekick>) == "justice_league");
static_assert(db::detail::table_from_type_name("Category") == "categories");
static_assert(db::detail::table_from_type_name("Day") == "days");
static_assert(db::detail::table_from_type_name("Box") == "boxes");
static_assert(db::detail::table_from_type_name("Match") == "matches");
static_assert(db::detail::table_from_type_name("Photo") == "photos");
static_assert(db::detail::table_from_type_name("HTTPRoute") == "http_routes");
static_assert(db::detail::table_from_type_name("Status") == "statuses");
static_assert(std::string_view(db::detail::Statements<Hero>::find) ==
              R"(select "id", "name", "alias", "mentor" from "heroes" where "id" = $1)");
static_assert(std::string_view(db::detail::Statements<Hero>::insert) ==
              R"(insert into "heroes" ("name", "alias", "mentor") values ($1, $2, $3) returning "id")");
static_assert(std::string_view(db::detail::Statements<Hero>::update) ==
              R"(update "heroes" set "name" = $1, "alias" = $2, "mentor" = $3 where "id" = $4)");
static_assert(db::Model<Hero>);
static_assert(json::omittable<Hero>::member("id") && !json::omittable<Hero>::member("name"));
struct NoKey { int x; };
static_assert(!db::Model<NoKey>);

namespace api {
[[= http::get("/heroes/{id}")]]
auto get(Db& db, std::int64_t id) -> std::optional<Hero> { return db.find<Hero>(id); }
[[= http::post("/heroes")]]
auto create(Db& db, Json<Hero> body) -> Created<Hero> { return {db.insert(*body)}; }
[[= http::get("/robins")]]
auto robins(Db& db) -> std::vector<Hero> { return db.where(&Hero::mentor, "Bruce Wayne").order_by(&Hero::id); }
}  // namespace api

static int run();
int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "uncaught: %s\n", e.what());
    return 1;
  }
}

static int run() {
  const char* url = std::getenv("CROCKET_TEST_DATABASE_URL");
  if (!url || !*url) {
    std::fprintf(stderr, "CROCKET_TEST_DATABASE_URL is not set: skipping\n");
    return 77;
  }
  db::Pool pool({.url = url, .pool_size = 2});
  auto d = pool.handle();
  d.exec("drop table if exists heroes, hero_teams, justice_league");
  d.exec("create table heroes (id bigserial primary key, name text not null unique, alias text not null, mentor text)");
  d.exec("create table justice_league (id bigserial primary key, name text not null, power int not null)");

  section("insert, find, update, erase");
  Hero flash{.name = "Barry Allen", .alias = "The Flash"};
  auto stored = d.insert(flash);
  CHECK(flash.id > 0);
  CHECK(stored.id == flash.id);
  Hero chosen{.id = 777'777, .name = "Jesse Quick", .alias = "Quick"};
  auto placed = d.insert(chosen);  // id is never sent: the database chooses
  CHECK(placed.id != 777'777 && chosen.id == placed.id);
  auto got = d.find<Hero>(flash.id);
  CHECK(got && got->name == "Barry Allen" && got->alias == "The Flash" && !got->mentor);
  CHECK(!d.find<Hero>(999'999));
  flash.alias = "Flash";
  CHECK(d.update(flash));
  CHECK(d.find<Hero>(flash.id)->alias == "Flash");
  Hero ghost{.id = 999'999, .name = "x", .alias = "y"};
  CHECK(!d.update(ghost));

  for (auto [n, a] : {std::pair{"Dick Grayson", "Robin"}, {"Jason Todd", "Robin II"}, {"Tim Drake", "Robin III"},
                      {"Damian Wayne", "Robin IV"}})
    d.insert(Hero{.name = n, .alias = a, .mentor = "Bruce Wayne"});
  auto wally = d.insert(Hero{.name = "Wally West", .alias = "Kid Flash", .mentor = "Barry Allen"});

  section("where");
  std::vector<Hero> robins = d.where(&Hero::mentor, "Bruce Wayne");
  CHECK(robins.size() == 4);
  auto first = d.where(&Hero::alias, db::like("Robin%")).order_by(&Hero::id).limit(1).first();
  CHECK(first && first->name == "Dick Grayson");
  auto last = d.where(&Hero::alias, db::like("Robin%")).order_by(&Hero::id, db::desc).first();
  CHECK(last && last->name == "Damian Wayne");
  CHECK(d.where(&Hero::mentor, std::nullopt).count() == 2);  // Barry and Jesse
  CHECK(d.where(&Hero::name, db::in({"Tim Drake", "Jason Todd"})).count() == 2);
  // Quotes, backslashes, commas and braces survive the array literal; ids work as numbers.
  auto odd = d.insert(Hero{.name = R"(Jean-Paul "Az" Valley, {\})", .alias = "Azrael", .mentor = "Bruce Wayne"});
  CHECK(d.where(&Hero::name, db::in({std::string(R"(Jean-Paul "Az" Valley, {\})"), std::string("Nobody")})).count() == 1);
  CHECK(d.where(&Hero::id, db::in({odd.id, wally.id})).count() == 2);
  CHECK(d.erase<Hero>(odd.id));
  CHECK(d.where(&Hero::id, db::gt(wally.id)).count() == 0);
  CHECK(d.where(&Hero::id, db::ge(wally.id)).count() == 1);
  CHECK(d.where(&Hero::mentor, "Bruce Wayne").where(&Hero::alias, db::ne("Robin")).count() == 3);
  int n = 0;
  for (auto& h : d.all<Hero>().order_by(&Hero::name).limit(2).offset(1)) n += h.id > 0;
  CHECK(n == 2);
  std::string mentor = "Barry Allen";  // a temporary's text is copied into the query
  auto q = d.where(&Hero::mentor, std::string(mentor));
  CHECK(q.all().size() == 1);

  section("erase, raw SQL, conflicts");
  CHECK(d.erase<Hero>(wally.id));
  CHECK(!d.erase<Hero>(wally.id));
  auto rows = d.sql<Hero>("select id, name, alias, mentor from heroes where name like $1 order by id", "%Wayne");
  CHECK(rows.size() == 1 && rows[0].name == "Damian Wayne" && rows[0].mentor == "Bruce Wayne");
  CHECK(d.exec("update heroes set alias = $1 where id = $2", "The Flash", flash.id).affected_rows() == 1);
  try {
    d.insert(Hero{.name = "Barry Allen", .alias = "dup"});
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK(e.status == 409 && e.code == "db.conflict");
  }
  CHECK(d.find<Hero>(flash.id)->alias == "The Flash");  // the connection is fine after an error

  section("table_name override");
  auto sk = d.insert(Sidekick{.name = "Krypto", .power = 9000});
  CHECK(d.find<Sidekick>(sk.id)->power == 9000);

  section("Db& in handlers: log line, metrics, readyz");
  std::mutex mu;
  std::vector<std::string> lines;
  Config cfg;
  cfg.log.sink = [&](std::string_view l) {
    std::lock_guard lk(mu);
    lines.emplace_back(l);
  };
  Metrics metrics;
  auto app = build(cfg);
  app.manage(std::move(pool)).attach(Logger{}).attach(metrics).mount("/", reflect_routes<^^api>());
  LocalClient client(app);
  auto r = client.get(std::format("/heroes/{}", flash.id)).dispatch();
  CHECK(r.status == 200 && contains(r.body, "Barry Allen"));
  CHECK(client.get("/heroes/424242").dispatch().status == 404);
  auto c = client.post("/heroes").json(R"({"id":0,"name":"Bart Allen","alias":"Impulse","mentor":"Barry Allen"})").dispatch();
  CHECK(c.status == 201 && contains(c.body, "Impulse"));
  // No id in the body: it reads as 0, and the database assigns one.
  auto noid = client.post("/heroes").json(R"({"name":"Jay Garrick","alias":"The Flash"})").dispatch();
  CHECK(noid.status == 201 && contains(noid.body, "Jay Garrick") && !contains(noid.body, R"("id":0)"));
  // A client-supplied id is ignored on insert.
  auto forced = client.post("/heroes").json(R"({"id":4242,"name":"Max Mercury","alias":"Max"})").dispatch();
  CHECK(forced.status == 201 && !contains(forced.body, R"("id":4242)"));
  CHECK(client.get("/heroes/4242").dispatch().status == 404);
  auto dup = client.post("/heroes").json(R"({"id":0,"name":"Bart Allen","alias":"Impulse","mentor":null})").dispatch();
  CHECK(dup.status == 409);
  auto rb = client.get("/robins").dispatch();
  CHECK(rb.status == 200 && contains(rb.body, "Dick Grayson") && contains(rb.body, "Damian Wayne"));
  CHECK(client.get("/readyz").dispatch().status == 200);
  bool logged = false;
  {
    std::lock_guard lk(mu);
    for (auto& l : lines)
      if (contains(l, R"("route":"/heroes/{id}")") && contains(l, R"("db_ms":)") && contains(l, R"("db_wait_ms":)") &&
          contains(l, R"("db_queries":1)"))
        logged = true;
    if (!logged)
      for (auto& l : lines) std::fprintf(stderr, "    %s\n", l.c_str());
  }
  CHECK(logged);
  auto m = client.get("/metrics").dispatch().body;
  CHECK(contains(m, R"(crocket_db_seconds_count{table="heroes",op="find"})"));
  CHECK(contains(m, R"(crocket_db_seconds_bucket{table="heroes",op="where",le="+Inf"})"));
  // The handle `d` from before still holds one of the two connections.
  CHECK(contains(m, R"(crocket_db_pool_connections{state="idle"} 1)"));
  CHECK(contains(m, R"(crocket_db_pool_connections{state="in_use"} 1)"));

  std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures ? 1 : 0;
}
