// The server ./dev bench drives with h2load: one route per scenario.
//
//   plaintext  GET  /plaintext                  the request pipeline with the least work
//   json       GET  /json                       encode a small struct
//   orders     POST /orders                     decode and re-encode a 1 KB order
//   router     GET  /api/v39/users/7/orders/9   matched last among ~200 routes
//   wait       GET  /wait                       the handler blocks its worker for 20 ms
//   wait_async GET  /wait_async                 the handler suspends for 20 ms (no worker held)
//
// plaintext_async, json_async, orders_async and router_async are the same as
// Task<T> handlers under /async: they run on the event loop, with no handoff.
//
//   CROCKET_PORT (18600), CROCKET_WORKERS and CROCKET_EVENT_LOOPS (0: the defaults), CROCKET_BENCH_LOGGER=1
//   to attach Logger (lines go to a sink that drops them), CROCKET_BENCH_LOGS=1 to keep warnings and
//   errors on stderr (adaptive placement's moves, for one) and expose GET /__routes.

#include <crocket/crocket.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

namespace bench {

struct Message {
  std::string message;
};
struct Item {
  std::string sku;
  std::string name;
  int qty = 1;
  double price = 0;
};
struct Order {
  std::uint64_t id;
  std::string email;
  std::vector<std::string> tags = {};
  std::vector<Item> items = {};
};

[[= http::get("/plaintext")]]
auto plaintext() -> std::string { return "Hello, World!"; }

[[= http::get("/json")]]
auto json() -> Json<Message> { return Json{Message{"Hello, World!"}}; }

[[= http::post("/orders")]]
auto orders(Json<Order> order) -> Json<Order> { return order; }

[[= http::get("/wait")]]
auto wait() -> std::string {
  std::this_thread::sleep_for(20ms);
  return "done";
}

[[= http::get("/wait_async")]]
auto wait_async() -> Task<std::string> {
  co_await sleep_for(20ms);
  co_return "done";
}

}  // namespace bench

namespace bench_async {

[[= http::get("/plaintext")]]
auto plaintext() -> Task<std::string> { co_return "Hello, World!"; }

[[= http::get("/json")]]
auto json() -> Task<Json<bench::Message>> { co_return Json{bench::Message{"Hello, World!"}}; }

[[= http::post("/orders")]]
auto orders(Json<bench::Order> order) -> Task<Json<bench::Order>> { co_return order; }

}  // namespace bench_async

namespace resource_async {

[[= http::get("/users/{id}/orders/{order}")]]
auto order(std::uint64_t id, std::uint64_t order) -> Task<std::string> { co_return std::to_string(id + order); }

}  // namespace resource_async

// Mounted at /api/v0 ... /api/v39: 200 routes the router scenario's target sits behind.
namespace resource {

[[= http::get("/users")]]
auto list() -> std::string { return "[]"; }

[[= http::get("/users/{id}")]]
auto get(std::uint64_t id) -> std::string { return std::to_string(id); }

[[= http::put("/users/{id}")]]
auto put(std::uint64_t id) -> std::string { return std::to_string(id); }

[[= http::get("/users/{id}/orders")]]
auto orders(std::uint64_t id) -> std::string { return std::to_string(id); }

[[= http::get("/users/{id}/orders/{order}")]]
auto order(std::uint64_t id, std::uint64_t order) -> std::string { return std::to_string(id + order); }

}  // namespace resource

static unsigned env_uint(const char* name, unsigned fallback) {
  const char* v = std::getenv(name);
  return v && *v ? unsigned(std::strtoul(v, nullptr, 10)) : fallback;
}

int main() {
  Config cfg;
  cfg.max_in_flight = 1'000'000;  // measure the server, not the admission limit
  if (env_uint("CROCKET_BENCH_LOGS", 0)) {
    cfg.log.level = log::Level::warn;
    cfg.debug_routes = true;
  } else {
    cfg.log.sink = [](std::string_view) {};
  }
  Crocket app{cfg};
  if (env_uint("CROCKET_BENCH_LOGGER", 0)) app.attach(Logger{});
  app.mount("/", reflect_routes<^^bench>());
  app.mount("/async", reflect_routes<^^bench_async>());
  for (int i = 0; i < 40; ++i) app.mount("/api/v" + std::to_string(i), reflect_routes<^^resource>());
  for (int i = 0; i < 40; ++i) app.mount("/async/api/v" + std::to_string(i), reflect_routes<^^resource_async>());
  return app.launch({.host = "127.0.0.1",
                     .port = std::uint16_t(env_uint("CROCKET_PORT", 18600)),
                     .workers = env_uint("CROCKET_WORKERS", 0),
                     .event_loops = env_uint("CROCKET_EVENT_LOOPS", 0),
                     .h2_prior_knowledge = true});
}
