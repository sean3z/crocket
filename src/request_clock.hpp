#pragma once
// One clock read for the end of a request, shared by everything that times it.
//
// The pipeline stamps the time once, when the handler returns (or when
// finish() starts, if nothing stamped it), and Logger, Metrics and the
// adaptive placement read that stamp instead of each calling the clock.

#include "crocket/request.hpp"

#include <chrono>
#include <string>

namespace crocket::detail {

struct RequestStamp {
  const Request* rq = nullptr;
  Clock::time_point received{};  // with rq: a later request at the same address is not mistaken for it
  Clock::time_point at{};
};
inline thread_local RequestStamp t_request_stamp;

/// Records `at` as the end of `rq` on this thread.
inline void stamp_request(const Request& rq, Clock::time_point at) { t_request_stamp = {&rq, rq.received, at}; }

/// Whether this thread has a stamp for `rq`.
inline bool stamped(const Request& rq) {
  return t_request_stamp.rq == &rq && t_request_stamp.received == rq.received;
}

/// The stamped end of `rq`, or now if this thread has none for it.
inline Clock::time_point response_time(const Request& rq) {
  return stamped(rq) ? t_request_stamp.at : Clock::now();
}

/// The wall-clock time for steady time `at`, from an offset this thread
/// refreshes once a second (so the system clock is read once a second, not
/// once a line).
std::chrono::system_clock::time_point wall_time(Clock::time_point at);

/// Appends `t` as "2026-10-07T16:53:20.123Z".
void append_iso8601(std::string& out, std::chrono::system_clock::time_point t);

}  // namespace crocket::detail
