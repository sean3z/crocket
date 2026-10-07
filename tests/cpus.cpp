// Counting physical cores for the default number of event loops, on fake
// /sys/devices/system/cpu trees.

#include "cpus.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using crocket::detail::physical_cores;

static int g_failures = 0, g_checks = 0;
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    auto va_ = (a);                                                                                 \
    auto vb_ = (b);                                                                                 \
    if (!(va_ == vb_)) {                                                                            \
      ++g_failures;                                                                                 \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lld)\n", __FILE__, __LINE__, #a, #b,       \
                   (long long)va_);                                                                 \
    }                                                                                               \
  } while (0)

/// cpuN -> (package, core)
static fs::path fake(const char* name, std::initializer_list<std::tuple<int, int, int>> cpus) {
  auto root = fs::temp_directory_path() / ("crocket-cpus-" + std::to_string(::getpid()) + "-" + name);
  fs::remove_all(root);
  for (auto [cpu, package, core] : cpus) {
    auto dir = root / ("cpu" + std::to_string(cpu)) / "topology";
    fs::create_directories(dir);
    std::ofstream(dir / "physical_package_id") << package << "\n";
    std::ofstream(dir / "core_id") << core << "\n";
  }
  return root;
}

int main() {
  // Hyperthreads: cpu0/cpu1 share core 0, cpu2/cpu3 share core 1.
  auto ht = fake("ht", {{0, 0, 0}, {1, 0, 0}, {2, 0, 1}, {3, 0, 1}});
  CHECK_EQ(physical_cores(ht, {0, 1, 2, 3}), 2u);
  CHECK_EQ(physical_cores(ht, {0, 1}), 1u);  // an affinity mask limits it
  CHECK_EQ(physical_cores(ht, {1, 2}), 2u);

  // Two sockets reuse core ids.
  auto two = fake("two", {{0, 0, 0}, {1, 0, 1}, {2, 1, 0}, {3, 1, 1}});
  CHECK_EQ(physical_cores(two, {0, 1, 2, 3}), 4u);

  // No hyperthreading: one core per CPU.
  auto flat = fake("flat", {{0, 0, 0}, {1, 0, 1}, {2, 0, 2}});
  CHECK_EQ(physical_cores(flat, {0, 1, 2}), 3u);

  // No topology (some containers): 0, and the default falls back.
  CHECK_EQ(physical_cores(fs::temp_directory_path() / "crocket-cpus-none", {0, 1}), 0u);

  unsigned loops = crocket::detail::default_event_loops();
  CHECK_EQ(loops >= 1 && loops <= std::max(1u, std::thread::hardware_concurrency()), true);
  std::fprintf(stderr, "this machine: %u event loops by default (%u logical CPUs)\n", loops,
               std::thread::hardware_concurrency());

  for (auto& p : {ht, two, flat}) fs::remove_all(p);
  std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
