// crocket_pod_saturation, crocket_dependency_saturation, and crocket_saturation.
// The formula is checked on its inputs. A Metrics render checks the same
// numbers come out of the gauges when worker counters are filled in directly.

#include <crocket/crocket.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace crocket;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                 \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    if (!(cond)) {                                                                                  \
      ++g_failures;                                                                                 \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
    }                                                                                               \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != hay.npos; }
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

static std::string render_with(std::int64_t busy_ns, std::int64_t dep_ns, bool deadline) {
  Crocket app;
  Metrics metrics;
  app.attach(metrics);
  auto ignited = app.ignite();
  if (!ignited) {
    std::fprintf(stderr, "%s\n", ignited.error().message().c_str());
    std::exit(1);
  }
  if (deadline) detail::note_dependency_deadline();
  auto& st = app.core().stats;
  st.worker_deadline = std::make_unique<std::atomic<std::int64_t>[]>(1);
  st.worker_usage = std::make_unique<detail::WorkerUsage[]>(1);
  st.worker_usage[0].busy_ns.store(busy_ns);
  st.worker_usage[0].dep_wait_ns.store(dep_ns);
  st.workers.store(1);
  return metrics.render();
}

int main() {
  using detail::saturation_gauges;
  constexpr std::int64_t kHuge = 10'000'000'000'000'000;  // clamps any real scrape window to 1

  section("formula");
  {  // Busy pod, healthy dependency: scale up.
    auto s = saturation_gauges(0.9, 0, 0, 0.05, 0, 0);
    CHECK(near(s.pod, 0.9));
    CHECK(near(s.dependency, 0.05));
    CHECK(near(s.value, 0.855));
  }
  {  // Slow or full dependency, pod almost idle: do not add pods.
    auto s = saturation_gauges(0.02, 0, 0, 1.0, 0, 0);
    CHECK(near(s.pod, 0.02));
    CHECK(s.dependency == 1.0);
    CHECK(s.value == 0.0);
  }
  {  // The queue can still be long because workers are stuck downstream. Still do not scale.
    auto s = saturation_gauges(0.02, 0.025, 0.050, 1.0, 0, 0);
    CHECK(s.pod == 1.0);
    CHECK(s.dependency == 1.0);
    CHECK(s.value == 0.0);
  }
  {  // No database and no deadlines: saturation is the pod gauge.
    auto s = saturation_gauges(0.42, 0, 0, 0, 0, 0);
    CHECK(s.dependency == 0.0);
    CHECK(s.value == s.pod);
    CHECK(near(s.pod, 0.42));
  }
  {  // Queue wait and loop lag are pod pressure, full at 25 ms and 50 ms.
    CHECK(saturation_gauges(0, 0.025, 0, 0, 0, 0).pod == 1.0);
    CHECK(near(saturation_gauges(0, 0.0125, 0, 0, 0, 0).pod, 0.5));
    CHECK(saturation_gauges(0, 0, 0.050, 0, 0, 0).pod == 1.0);
    CHECK(near(saturation_gauges(0, 0, 0.025, 0, 0, 0).pod, 0.5));
  }
  {  // One deadline in the window saturates the dependency even with pool room.
    auto s = saturation_gauges(0.8, 0, 0, 0.3, 0.1, 1);
    CHECK(s.dependency == 1.0);
    CHECK(s.value == 0.0);
    auto open = saturation_gauges(0.8, 0, 0, 0.3, 0.1, 0);
    CHECK(near(open.dependency, 0.3));
    CHECK(near(open.value, 0.8 * 0.7));
  }
  {  // Inputs outside 0..1 are clamped before the product.
    auto s = saturation_gauges(2, 0, 0, 2, 0, 0);
    CHECK(s.pod == 1.0);
    CHECK(s.dependency == 1.0);
    CHECK(s.value == 0.0);
    CHECK(saturation_gauges(-1, -1, -1, -1, -1, 0).value == 0.0);
  }

  section("busy pod, no dependency");
  {
    auto body = render_with(kHuge, 0, false);
    CHECK(contains(body, "# TYPE crocket_worker_busy_ratio gauge\ncrocket_worker_busy_ratio 1.0000\n"));
    CHECK(contains(body, "# TYPE crocket_pod_saturation gauge\ncrocket_pod_saturation 1.0000\n"));
    CHECK(contains(body, "# TYPE crocket_dependency_saturation gauge\ncrocket_dependency_saturation 0.0000\n"));
    CHECK(contains(body, "# TYPE crocket_saturation gauge\ncrocket_saturation 1.0000\n"));
  }

  section("workers blocked on a dependency");
  {
    // Handler time is entirely the dependency, so the busy ratio stays high and
    // pod saturation does not.
    auto body = render_with(kHuge, kHuge, false);
    CHECK(contains(body, "crocket_worker_busy_ratio 1.0000\n"));
    CHECK(contains(body, "crocket_pod_saturation 0.0000\n"));
    CHECK(contains(body, "crocket_dependency_saturation 1.0000\n"));
    CHECK(contains(body, "crocket_saturation 0.0000\n"));
  }

  section("dependency deadline dampens a busy pod");
  {
    auto body = render_with(kHuge, 0, true);
    CHECK(contains(body, "crocket_pod_saturation 1.0000\n"));
    CHECK(contains(body, "crocket_dependency_saturation 1.0000\n"));
    CHECK(contains(body, "crocket_saturation 0.0000\n"));
  }

  std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures ? 1 : 0;
}
