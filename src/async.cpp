#include "crocket/async.hpp"

#include "crocket/app.hpp"

#include <condition_variable>
#include <map>
#include <thread>

namespace crocket::detail {
namespace {

/// One thread for every sleep_for in the process.
class Timers {
 public:
  void add(Sleeper* s) {
    {
      std::lock_guard lk(mu_);
      by_time_.emplace(s->until, s);
      if (!thread_.joinable()) thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }
    cv_.notify_one();
  }
  /// Removes `s` if it is still waiting. True if this call took it off.
  bool remove(Sleeper* s) {
    std::lock_guard lk(mu_);
    auto [first, last] = by_time_.equal_range(s->until);
    for (auto it = first; it != last; ++it)
      if (it->second == s) {
        by_time_.erase(it);
        return true;
      }
    return false;
  }

 private:
  void run(std::stop_token st) {
    std::unique_lock lk(mu_);
    while (!st.stop_requested()) {
      if (by_time_.empty()) {
        cv_.wait(lk, st, [&] { return !by_time_.empty(); });
        continue;
      }
      auto next = by_time_.begin()->first;
      if (cv_.wait_until(lk, st, next, [&] { return by_time_.empty() || by_time_.begin()->first < next; })) continue;
      // Everything due: off the map under the lock, woken outside it.
      std::vector<Sleeper*> due;
      auto now = std::chrono::steady_clock::now();
      while (!by_time_.empty() && by_time_.begin()->first <= now) {
        due.push_back(by_time_.begin()->second);
        by_time_.erase(by_time_.begin());
      }
      lk.unlock();
      for (auto* s : due) fire(s);
      lk.lock();
    }
  }

  std::mutex mu_;
  std::condition_variable_any cv_;
  std::multimap<std::chrono::steady_clock::time_point, Sleeper*> by_time_;
  std::jthread thread_;
};

Timers& timers() {
  static auto* t = new Timers;  // never destroyed: see fallback_pool() in app.cpp
  return *t;
}

}  // namespace

/// Resumes the sleeper once, whoever gets here first (the timer or a cancellation).
void fire(Sleeper* s) {
  int st = s->state.load();
  while (st != Sleeper::Fired)
    if (s->state.compare_exchange_weak(st, Sleeper::Fired)) {
      if (st == Sleeper::Armed) resume_on_worker(s->exchange, s->handle);
      return;  // Registering: await_suspend sees Fired and does not suspend
    }
}

void timer_add(Sleeper* s) { timers().add(s); }

void timer_wake(Sleeper* s) {
  timers().remove(s);
  fire(s);
}

std::stop_token current_stop_token() {
  Exchange* ex = current_exchange();
  return ex ? ex->req.deadline.stop : std::stop_token{};
}

}  // namespace crocket::detail
