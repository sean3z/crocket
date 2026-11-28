#pragma once
// std::move_only_function where the standard library has it. libc++ in
// clang-p2996 leaves __cpp_lib_move_only_function undefined, so there the
// same name is a small move-only stand-in. libstdc++ keeps the standard type.

#include <version>
#include <memory>
#include <type_traits>
#include <utility>
#if defined(__cpp_lib_move_only_function)
#include <functional>
#endif

namespace crocket::detail {

#if defined(__cpp_lib_move_only_function)
template <class Sig>
using move_only_function = std::move_only_function<Sig>;
#else
template <class>
class move_only_function;

template <class R, class... Args>
class move_only_function<R(Args...)> {
  struct Base {
    virtual R call(Args&&...) = 0;
    virtual ~Base() = default;
  };
  template <class F>
  struct Impl final : Base {
    F f;
    explicit Impl(F fun) : f(std::move(fun)) {}
    R call(Args&&... args) override { return f(std::forward<Args>(args)...); }
  };
  std::unique_ptr<Base> fn_;

 public:
  move_only_function() = default;
  move_only_function(move_only_function&&) noexcept = default;
  move_only_function& operator=(move_only_function&&) noexcept = default;
  move_only_function(const move_only_function&) = delete;
  move_only_function& operator=(const move_only_function&) = delete;

  template <class F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, move_only_function> &&
             std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
  move_only_function(F&& f) : fn_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

  explicit operator bool() const noexcept { return fn_ != nullptr; }
  R operator()(Args... args) { return fn_->call(std::forward<Args>(args)...); }
};
#endif

}  // namespace crocket::detail
