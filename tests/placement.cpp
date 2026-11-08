// Adaptive placement, with the durations supplied here. tls_h2 still checks a
// live server: a plain route starts on workers, and a Pool route stays there.
// Promoting on that server needs 1,000 handler runs in a row under 100 us of
// wall time, and a shared runner's scheduler breaks the streak.

#include <crocket/crocket.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using namespace crocket;
using namespace std::chrono_literals;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                 \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    if (!(cond)) {                                                                                  \
      ++g_failures;                                                                                 \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
    }                                                                                               \
  } while (0)

namespace place {
[[= http::get("/fast")]]
auto fast() -> std::string { return "ok"; }

struct Gate {
  static constexpr bool crocket_may_block = true;
};
[[= http::get("/gated")]]
auto gated(State<Gate> gate) -> std::string {
  (void)gate;
  return "held";
}
}  // namespace place

static const RouteDef& find_route(const Crocket& app, std::string_view path) {
  for (auto& r : app.routes())
    if (r.path == path) return r;
  std::fprintf(stderr, "missing route %.*s\n", int(path.size()), path.data());
  std::exit(1);
}

static void fast_samples(detail::Placement& p, int n) {
  for (int i = 0; i < n; ++i) p.note_worker("api::fast", 50us);
}

int main() {
  std::mutex mu;
  std::string log;
  Config cfg;
  cfg.log.sink = [&](std::string_view line) {
    std::lock_guard lk(mu);
    log.append(line);
    log.push_back('\n');
  };
  Crocket app(cfg);
  app.manage(place::Gate{});
  app.mount("/", reflect_routes<^^place>());
  auto ignited = app.ignite();
  CHECK(ignited.has_value());
  if (!ignited) {
    std::fprintf(stderr, "%s\n", ignited.error().message().c_str());
    return 1;
  }

  auto& plain = find_route(app, "/fast");
  auto& blocked = find_route(app, "/gated");
  auto& plain_place = app.core().placement[std::size_t(&plain - app.routes().data())];
  CHECK(!plain.may_block);
  CHECK(blocked.may_block);
  CHECK(!app.loop_route(http::Method::Get, "/fast"));
  CHECK(!app.loop_route(http::Method::Get, "/gated"));

  fast_samples(plain_place, int(detail::Placement::kPromoteAfter) - 1);
  CHECK(!plain_place.on_loop.load());
  CHECK(!app.loop_route(http::Method::Get, "/fast"));
  plain_place.note_worker("api::fast", 50us);
  app.flush_logs();
  CHECK(plain_place.on_loop.load());
  CHECK(app.loop_route(http::Method::Get, "/fast"));
  {
    std::lock_guard lk(mu);
    CHECK(log.find("api::fast runs on the event loops from now on") != std::string::npos);
  }

  // At the fast limit the streak starts over. 999 more do not promote.
  plain_place.on_loop.store(false);
  plain_place.fast_runs.store(0);
  plain_place.retry_at.store(0);
  fast_samples(plain_place, 400);
  plain_place.note_worker("api::fast", detail::Placement::kFast);
  fast_samples(plain_place, int(detail::Placement::kPromoteAfter) - 1);
  CHECK(!plain_place.on_loop.load());
  plain_place.note_worker("api::fast", 50us);
  CHECK(plain_place.on_loop.load());

  // One sample over 1 ms, under a stall, stays on the loop.
  plain_place.note_loop("api::fast", 5ms);
  CHECK(plain_place.on_loop.load());

  // Seven more inside the same window make eight, and that demotes.
  for (int i = 0; i < int(detail::Placement::kStrikes) - 1; ++i) plain_place.note_loop("api::fast", 5ms);
  app.flush_logs();
  CHECK(!plain_place.on_loop.load());
  {
    std::lock_guard lk(mu);
    CHECK(log.find("api::fast blocked its event loop") != std::string::npos);
    CHECK(log.find("after 8 slow runs within 1 s") != std::string::npos);
  }

  // The retry hold blocks the next promotion. Clearing it allows another.
  fast_samples(plain_place, int(detail::Placement::kPromoteAfter));
  CHECK(!plain_place.on_loop.load());
  plain_place.retry_at.store(0);
  plain_place.fast_runs.store(0);
  fast_samples(plain_place, int(detail::Placement::kPromoteAfter));
  CHECK(plain_place.on_loop.load());

  // One stall demotes immediately, and says it was a single slow run.
  std::size_t before;
  {
    std::lock_guard lk(mu);
    before = log.size();
  }
  plain_place.note_loop("api::fast", detail::Placement::kStall + 1ms);
  app.flush_logs();
  CHECK(!plain_place.on_loop.load());
  {
    std::lock_guard lk(mu);
    CHECK(log.find("after 1 slow runs within 1 s", before) != std::string::npos);
  }

  // A route that may block never runs on the loop, even if the flag is set.
  auto& blocked_place = app.core().placement[std::size_t(&blocked - app.routes().data())];
  blocked_place.on_loop.store(true);
  CHECK(!app.loop_route(http::Method::Get, "/gated"));

  // Two threads counting fast samples still promote exactly once.
  plain_place.on_loop.store(false);
  plain_place.fast_runs.store(0);
  plain_place.retry_at.store(0);
  std::thread a([&] { fast_samples(plain_place, 600); });
  std::thread b([&] { fast_samples(plain_place, 600); });
  a.join();
  b.join();
  CHECK(plain_place.on_loop.load());

  std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
