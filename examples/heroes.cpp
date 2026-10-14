// Heroes: a CRUD API on Postgres with crocket::db.
//
//   createdb heroes
//   CROCKET_DATABASE_URL=postgresql://localhost/heroes ./build/crocket_heroes
//   curl localhost:8000/heroes/1
//   curl 'localhost:8000/heroes?mentor=Bruce%20Wayne'
//   curl -X POST localhost:8000/heroes -H 'content-type: application/json' \
//        -d '{"id":0,"name":"Bart Allen","alias":"Impulse","mentor":"Barry Allen"}'
//   curl localhost:8000/metrics | grep crocket_db_seconds

#include <crocket/crocket.hpp>
#include <crocket/db.hpp>

#include <cstdint>
#include <optional>
#include <string>

using namespace crocket;

struct Hero {
  std::int64_t id;
  std::string name;
  std::string alias;
  std::optional<std::string> mentor;
};

struct HeroQuery {
  std::optional<std::string> mentor;
};

namespace api {

[[= http::get("/heroes")]]
auto list(Db& db, Query<HeroQuery> q) -> Json<std::vector<Hero>> {
  if (q->mentor) return {db.where(&Hero::mentor, *q->mentor).order_by(&Hero::id).all()};
  return {db.all<Hero>().order_by(&Hero::id).limit(100).all()};
}

[[= http::get("/heroes/{id}")]]
auto get(Db& db, std::int64_t id) -> std::optional<Json<Hero>> {
  if (auto h = db.find<Hero>(id)) return Json<Hero>{*h};
  return std::nullopt;
}

[[= http::post("/heroes")]]
auto create(Db& db, Json<Hero> body) -> Created<Json<Hero>> { return {Json<Hero>{db.insert(*body)}}; }

[[= http::put("/heroes/{id}")]]
auto replace(Db& db, std::int64_t id, Json<Hero> body) -> std::optional<Json<Hero>> {
  body->id = id;
  if (!db.update(*body)) return std::nullopt;
  return body;
}

[[= http::del("/heroes/{id}")]]
auto remove(Db& db, std::int64_t id) -> std::optional<NoContent> {
  if (!db.erase<Hero>(id)) return std::nullopt;
  return NoContent{};
}

}  // namespace api

int main() {
  auto cfg = Config::from_env();
  db::Pool pool(cfg.database);

  auto db = pool.handle();
  db.exec("create table if not exists heroes (id bigserial primary key, name text not null unique, "
          "alias text not null, mentor text)");
  if (db.all<Hero>().count() == 0) {
    db.insert(Hero{.name = "Barry Allen", .alias = "The Flash"});
    db.insert(Hero{.name = "Wally West", .alias = "Kid Flash", .mentor = "Barry Allen"});
    for (auto [name, alias] : {std::pair{"Dick Grayson", "Robin"}, {"Jason Todd", "Robin"},
                               {"Tim Drake", "Robin"}, {"Damian Wayne", "Robin"}})
      db.insert(Hero{.name = name, .alias = alias, .mentor = "Bruce Wayne"});
  }

  return build(cfg)
      .manage(std::move(pool))
      .attach(Logger{})
      .attach(Metrics{})
      .mount("/", reflect_routes<^^api>())
      .launch();
}
