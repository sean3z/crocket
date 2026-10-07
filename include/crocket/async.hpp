#pragma once
// Awaitables for Task<T> handlers that free their thread while they wait.
//
//   [[= http::get("/quote/{sym}")]]
//   auto quote(std::string sym) -> Task<std::string> {
//     auto [price, resolve] = callback<std::string>();
//     prices.fetch(sym, [resolve](std::string p) { resolve(std::move(p)); });  // any callback API
//     co_return co_await std::move(price);
//   }
//
// Both resume the task where it runs (its event loop) with the request's context.

#include "crocket/task.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace crocket {

namespace detail {
/// One pending sleep, registered with the timer thread.
struct Sleeper {
  enum : int { Registering, Armed, Fired };
  std::chrono::steady_clock::time_point until;
  std::coroutine_handle<> handle;
  Exchange* exchange = nullptr;
  // Registering -> Armed (suspended) -> Fired (resumed). Woken while still
  // Registering, the task does not suspend at all, so nothing resumes it from
  // another thread while await_suspend still runs.
  std::atomic<int> state{Registering};
};
void timer_add(Sleeper* s);
void fire(Sleeper* s);
/// Takes `s` off the timer and wakes it, if it has not fired yet.
void timer_wake(Sleeper* s);
std::stop_token current_stop_token();
}  // namespace detail

/// `co_await sleep_for(100ms)`: waits without holding a worker. Ends early when
/// the request is cancelled (deadline, client gone, shutdown); check
/// Deadline::expired() afterwards if that matters.
class [[nodiscard]] SleepFor {
 public:
  static constexpr bool crocket_resumes_on_worker = true;
  explicit SleepFor(std::chrono::steady_clock::duration d) : d_(d) {}
  bool await_ready() const noexcept { return d_ <= std::chrono::steady_clock::duration::zero(); }
  bool await_suspend(std::coroutine_handle<> h) {
    s_.until = std::chrono::steady_clock::now() + d_;
    s_.handle = h;
    s_.exchange = detail::current_exchange();
    detail::timer_add(&s_);
    if (auto token = detail::current_stop_token(); token.stop_possible())
      cancel_.emplace(token, Wake{&s_});  // runs Wake now if the request is already cancelled
    int expected = detail::Sleeper::Registering;
    if (s_.state.compare_exchange_strong(expected, detail::Sleeper::Armed)) return true;
    detail::timer_wake(&s_);  // woken while registering: off the timer, and carry on without suspending
    return false;
  }
  void await_resume() noexcept { cancel_.reset(); }

 private:
  struct Wake {
    detail::Sleeper* s;
    void operator()() const noexcept { detail::timer_wake(s); }
  };
  std::chrono::steady_clock::duration d_;
  detail::Sleeper s_;
  std::optional<std::stop_callback<Wake>> cancel_;
};

inline SleepFor sleep_for(std::chrono::steady_clock::duration d) { return SleepFor(d); }

namespace detail {
template <class T>
struct CallbackState {
  std::mutex mu;
  std::optional<T> value;
  bool broken = false;  // every resolver went away without a value
  std::coroutine_handle<> waiter;
  Exchange* exchange = nullptr;
  std::atomic<int> resolvers{0};

  /// Stores the outcome; resumes the waiting task if there is one.
  void settle(std::optional<T> v) {
    std::coroutine_handle<> h;
    Exchange* ex;
    {
      std::lock_guard lk(mu);
      if (value || broken) return;  // settled already
      if (v) value = std::move(v);
      else broken = true;
      h = std::exchange(waiter, nullptr);
      ex = exchange;
    }
    if (h) resume_on_worker(ex, h);
  }
};
}  // namespace detail

template <class T>
class Resolve;

/// The awaiting side of callback<T>(): `T v = co_await std::move(future);`.
/// Throws std::runtime_error if every Resolve is destroyed without a value.
template <class T>
class [[nodiscard]] Future {
 public:
  static constexpr bool crocket_resumes_on_worker = true;
  bool await_ready() const {
    std::lock_guard lk(s_->mu);
    return s_->value || s_->broken;
  }
  bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard lk(s_->mu);
    if (s_->value || s_->broken) return false;  // settled between await_ready and here
    s_->waiter = h;
    s_->exchange = detail::current_exchange();
    return true;
  }
  T await_resume() {
    std::lock_guard lk(s_->mu);
    if (!s_->value) throw std::runtime_error("crocket::callback: resolved by nobody (every Resolve was destroyed)");
    return std::move(*s_->value);
  }

 private:
  template <class U>
  friend std::pair<Future<U>, Resolve<U>> callback();
  explicit Future(std::shared_ptr<detail::CallbackState<T>> s) : s_(std::move(s)) {}
  std::shared_ptr<detail::CallbackState<T>> s_;
};

/// The completing side of callback<T>(): call it once, from any thread, with
/// the value. Copyable; later calls are ignored.
template <class T>
class Resolve {
 public:
  Resolve(const Resolve& o) : s_(o.s_) { s_->resolvers.fetch_add(1); }
  Resolve(Resolve&& o) noexcept : s_(std::move(o.s_)) {}
  Resolve& operator=(Resolve o) noexcept {
    std::swap(s_, o.s_);
    return *this;
  }
  ~Resolve() {
    if (s_ && s_->resolvers.fetch_sub(1) == 1) s_->settle(std::nullopt);  // the last one, unresolved
  }
  void operator()(T value) const { s_->settle(std::move(value)); }

 private:
  template <class U>
  friend std::pair<Future<U>, Resolve<U>> callback();
  explicit Resolve(std::shared_ptr<detail::CallbackState<T>> s) : s_(std::move(s)) { s_->resolvers.fetch_add(1); }
  std::shared_ptr<detail::CallbackState<T>> s_;
};

/// A Future/Resolve pair, for libraries that report results through a callback.
template <class T>
std::pair<Future<T>, Resolve<T>> callback() {
  auto s = std::make_shared<detail::CallbackState<T>>();
  return {Future<T>(s), Resolve<T>(s)};
}

}  // namespace crocket
