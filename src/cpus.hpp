#pragma once
// How many event loops to run by default: one per physical core this process
// may use. Two loops on the hyperthreads of one core compete for it, and a
// loop that waits for a CPU stalls every connection it owns.

#include <string>
#include <vector>

namespace crocket::detail {

/// The CPUs this process may run on (its affinity mask).
std::vector<int> allowed_cpus();

/// Distinct physical cores among `cpus`, from `<sys_cpu_dir>/cpuN/topology`
/// (physical_package_id, core_id). 0 if the topology cannot be read.
unsigned physical_cores(const std::string& sys_cpu_dir, const std::vector<int>& cpus);

/// physical_cores() for this process, or the logical CPU count without topology.
unsigned default_event_loops();

}  // namespace crocket::detail
