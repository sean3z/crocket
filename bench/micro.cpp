// In-process microbenchmarks: time and heap allocations per operation, with no
// sockets, so small changes show. Run with ./dev bench micro.

#include <crocket/crocket.hpp>

#include "router.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// Every allocation in the process is counted.
static std::atomic<std::uint64_t> g_allocs{0};
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace crocket;

namespace bench {
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
}  // namespace bench

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

namespace {

/// Runs `op` for about `budget`, then prints one row: name, ns/op, allocs/op.
/// The output is tab-separated for ./dev bench.
template <class F>
void measure(const char* name, F op, std::chrono::milliseconds budget = std::chrono::milliseconds(500)) {
  for (int i = 0; i < 1000; ++i) op();  // warm up
  std::uint64_t n = 0;
  auto a0 = g_allocs.load();
  auto t0 = std::chrono::steady_clock::now();
  auto until = t0 + budget;
  while (std::chrono::steady_clock::now() < until) {
    for (int i = 0; i < 256; ++i) op();
    n += 256;
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
  double allocs = double(g_allocs.load() - a0) / double(n);
  std::printf("%s\tns_per_op\t%.1f\tlower\n%s\tallocs_per_op\t%.2f\tlower\n", name, ns, name, allocs);
}

std::string order_json() {
  bench::Order o{1001, "customer@example.com", {"priority", "gift", "eu"}, {}};
  for (int i = 0; i < 8; ++i) o.items.push_back({"SKU-" + std::to_string(i * 7919), "Widget model " + std::to_string(i), i + 1, 9.99 + i});
  return json::to_string(o);
}

}  // namespace

int main() {
  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  Crocket app{cfg};
  app.mount("/", reflect_routes<^^bench>());
  for (int i = 0; i < 40; ++i) app.mount("/api/v" + std::to_string(i), reflect_routes<^^resource>());
  LocalClient client(app);

  detail::Router router(app.routes());
  volatile std::size_t sink = 0;
  measure("router.first", [&] { sink = sink + router.match(http::Method::Get, "/plaintext").size(); });
  measure("router.last", [&] { sink = sink + router.match(http::Method::Get, "/api/v39/users/7/orders/9").size(); });
  measure("router.miss", [&] { sink = sink + router.match(http::Method::Get, "/api/v39/nothing/here").size(); });

  measure("pipeline.plaintext", [&] { sink = sink + client.get("/plaintext").dispatch().body.size(); });
  measure("pipeline.router", [&] { sink = sink + client.get("/api/v39/users/7/orders/9").dispatch().body.size(); });

  auto text = order_json();
  measure("json.read_order", [&] {
    bench::Order o{};
    sink = sink + json::read(text, o).has_value();
  });
  bench::Order parsed = json::from_string<bench::Order>(text).value();
  measure("json.write_order", [&] { sink = sink + json::to_string(parsed).size(); });
  return sink == 0xdeadbeef;
}
