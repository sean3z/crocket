#include "cpus.hpp"

#include <sched.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <thread>
#include <utility>

namespace crocket::detail {

std::vector<int> allowed_cpus() {
  std::vector<int> out;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return out;
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &set)) out.push_back(i);
  return out;
}

unsigned physical_cores(const std::string& sys_cpu_dir, const std::vector<int>& cpus) {
  std::set<std::pair<long, long>> cores;  // (package, core)
  for (int cpu : cpus) {
    auto dir = sys_cpu_dir + "/cpu" + std::to_string(cpu) + "/topology/";
    long package = -1, core = -1;
    std::ifstream(dir + "physical_package_id") >> package;
    std::ifstream(dir + "core_id") >> core;
    if (core < 0) return 0;  // no topology: let the caller fall back
    cores.emplace(package, core);
  }
  return unsigned(cores.size());
}

unsigned default_event_loops() {
  if (unsigned n = physical_cores("/sys/devices/system/cpu", allowed_cpus())) return n;
  return std::max(1u, std::thread::hardware_concurrency());
}

}  // namespace crocket::detail
