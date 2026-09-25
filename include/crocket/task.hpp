#pragma once
// Task<T>: a lazy coroutine type handlers may return instead of T.
// The router detects an awaitable result and awaits it on the worker thread
// that runs the request; there is no separate async route API.

#include <coroutine>
#include <exception>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>

namespace crocket {

template <class T = void>
class Task;

namespace detail {

struct TaskPromiseBase {
  std::coroutine_handle<> continuation = std::noop_coroutine();
  std::exception_ptr error;

  std::suspend_always initial_suspend() noexcept { return {}; }
  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
      return h.promise().continuation;
    }
    void await_resume() noexcept {}
  };
  FinalAwaiter final_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class T>
struct TaskPromise : TaskPromiseBase {
  std::optional<T> value;
  Task<T> get_return_object() noexcept;
  template <class U = T>
  void return_value(U&& v) { value.emplace(std::forward<U>(v)); }
  T take() {
    if (error) std::rethrow_exception(error);
    return std::move(*value);
  }
};

template <>
struct TaskPromise<void> : TaskPromiseBase {
  Task<void> get_return_object() noexcept;
  void return_void() noexcept {}
  void take() {
    if (error) std::rethrow_exception(error);
  }
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::TaskPromise<T>;
  using value_type = T;

  explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
  Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  Task& operator=(Task&& o) noexcept {
    if (this != &o) { reset(); h_ = std::exchange(o.h_, {}); }
    return *this;
  }
  ~Task() { reset(); }

  bool await_ready() const noexcept { return !h_ || h_.done(); }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
    h_.promise().continuation = caller;
    return h_;
  }
  T await_resume() { return h_.promise().take(); }

 private:
  void reset() {
    if (h_) h_.destroy();
    h_ = {};
  }
  std::coroutine_handle<promise_type> h_;
};

namespace detail {
template <class T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}
inline Task<void> TaskPromise<void>::get_return_object() noexcept {
  return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

/// Anything with await_ready/await_suspend/await_resume.
template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h) {
  { a.await_ready() } -> std::convertible_to<bool>;
  a.await_suspend(h);
  a.await_resume();
};
template <class A>
concept Awaitable = Awaiter<A> || requires(A a) { { std::move(a).operator co_await() } -> Awaiter; };

template <class A>
using await_result_t = decltype([] {
  if constexpr (Awaiter<A>) return std::type_identity<decltype(std::declval<A&>().await_resume())>{};
  else return std::type_identity<decltype(std::declval<A>().operator co_await().await_resume())>{};
}())::type;

struct SyncWaitDriver {
  struct promise_type {
    std::binary_semaphore* done;
    // Coroutine parameters are passed to the promise constructor; the first
    // one is the semaphore to release when the driver finishes.
    template <class... Rest>
    explicit promise_type(std::binary_semaphore& s, Rest&...) noexcept : done(&s) {}
    SyncWaitDriver get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    auto final_suspend() noexcept {
      struct Signal {
        std::binary_semaphore* s;
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
          auto* sem = s;
          h.destroy();
          sem->release();
        }
        void await_resume() noexcept {}
      };
      return Signal{done};
    }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };
};

template <class R, class A, class Out>
SyncWaitDriver sync_wait_run(std::binary_semaphore& done, A& a, std::exception_ptr& err, Out& out) {
  (void)done;
  try {
    if constexpr (std::is_void_v<R>) {
      co_await std::move(a);
      out.emplace('\0');
    } else {
      out.emplace(co_await std::move(a));
    }
  } catch (...) {
    err = std::current_exception();
  }
}
}  // namespace detail

/// Blocks the calling thread until the awaitable completes. If the awaitable
/// completes synchronously everything runs inline on this thread.
template <detail::Awaitable A>
auto sync_wait(A&& awaitable) -> detail::await_result_t<std::remove_cvref_t<A>> {
  using R = detail::await_result_t<std::remove_cvref_t<A>>;
  std::binary_semaphore done{0};
  std::exception_ptr error;
  std::optional<std::conditional_t<std::is_void_v<R>, char, std::remove_reference_t<R>>> result;
  detail::sync_wait_run<R>(done, awaitable, error, result);
  done.acquire();
  if (error) std::rethrow_exception(error);
  if constexpr (!std::is_void_v<R>) return std::move(*result);
}

}  // namespace crocket
