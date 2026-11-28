#pragma once
// std::atomic<std::shared_ptr<T>> where the library has it (libstdc++).
// clang-p2996's libc++ leaves __cpp_lib_atomic_shared_ptr undefined, so there
// the same operations take a mutex. The GCC path is the standard type.

#include <version>
#include <atomic>
#include <memory>
#include <mutex>

namespace crocket::detail {

template <class T>
class atomic_shared_ptr {
 public:
  atomic_shared_ptr() = default;

  void store(std::shared_ptr<T> next, std::memory_order order = std::memory_order_seq_cst) {
#if defined(__cpp_lib_atomic_shared_ptr)
    value_.store(std::move(next), order);
#else
    (void)order;
    std::lock_guard lock(mu_);
    value_ = std::move(next);
#endif
  }

  std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const {
#if defined(__cpp_lib_atomic_shared_ptr)
    return value_.load(order);
#else
    (void)order;
    std::lock_guard lock(mu_);
    return value_;
#endif
  }

  bool compare_exchange_strong(std::shared_ptr<T>& expected, std::shared_ptr<T> desired,
                               std::memory_order order = std::memory_order_seq_cst) {
#if defined(__cpp_lib_atomic_shared_ptr)
    return value_.compare_exchange_strong(expected, std::move(desired), order);
#else
    (void)order;
    std::lock_guard lock(mu_);
    if (value_.get() != expected.get()) {
      expected = value_;
      return false;
    }
    value_ = std::move(desired);
    return true;
#endif
  }

 private:
#if defined(__cpp_lib_atomic_shared_ptr)
  std::atomic<std::shared_ptr<T>> value_;
#else
  mutable std::mutex mu_;
  std::shared_ptr<T> value_;
#endif
};

}  // namespace crocket::detail
