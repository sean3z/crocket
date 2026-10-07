#pragma once
// Pool<T>: a bounded set of reusable resources (DB connections, clients).
// checkout() waits until the request's deadline, honours cancellation, and
// fails with 503 db.busy. Managed pools with a ready() member are picked up by
// GET /readyz automatically.

#include "crocket/request.hpp"

#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <vector>

namespace crocket {

template <class T>
class Pool {
 public:
  /// checkout() waits when every connection is out: handlers using a Pool stay
  /// on workers, never on an event loop (Config::adaptive_placement).
  static constexpr bool crocket_may_block = true;

 private:
  struct Shared {
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<std::unique_ptr<T>> idle;
    std::size_t size = 0;
  };

 public:
  struct Options {
    std::string_view busy_code = "db.busy";
    Clock::duration max_wait = std::chrono::seconds(5);         // cap even if deadline is later
    Clock::duration ready_wait = std::chrono::milliseconds(250);  // /readyz checkout budget
    std::function<bool(T&)> health = {};                         // optional probe for /readyz
  };

  /// Returns the resource to the pool when destroyed.
  class Lease {
   public:
    Lease(Lease&&) noexcept = default;
    /// Returns the resource this lease holds before taking over `o`'s.
    Lease& operator=(Lease&& o) noexcept {
      if (this != &o) {
        release();
        shared_ = std::move(o.shared_);
        item_ = std::move(o.item_);
      }
      return *this;
    }
    ~Lease() { release(); }
    T& operator*() const { return *item_; }
    T* operator->() const { return item_.get(); }

   private:
    friend class Pool;
    Lease(std::shared_ptr<Shared> s, std::unique_ptr<T> i) : shared_(std::move(s)), item_(std::move(i)) {}
    void release() noexcept {
      if (!item_) return;
      {
        std::lock_guard lk(shared_->mu);
        shared_->idle.push_back(std::move(item_));
      }
      shared_->cv.notify_one();
    }
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<T> item_;
  };

  Pool(std::size_t n, const std::function<T()>& make, Options opts = {})
      : shared_(std::make_shared<Shared>()), opts_(std::move(opts)) {
    for (std::size_t i = 0; i < n; ++i) shared_->idle.push_back(std::make_unique<T>(make()));
    shared_->size = n;
  }
  Pool(Pool&&) noexcept = default;

  /// Waits for a free resource until the deadline (bounded by max_wait).
  std::expected<Lease, ApiError> checkout(const Deadline& d) const {
    auto until = std::min(d.at, Clock::now() + opts_.max_wait);
    std::unique_lock lk(shared_->mu);
    bool got = shared_->cv.wait_until(lk, d.stop, until, [&] { return !shared_->idle.empty(); });
    if (!got)
      return std::unexpected(ApiError{503, opts_.busy_code, "service busy, try again",
                                      d.stop.stop_requested() ? "pool wait cancelled" : "pool wait timed out"});
    auto item = std::move(shared_->idle.front());
    shared_->idle.pop_front();
    return Lease(shared_, std::move(item));
  }

  /// Readiness probe used by /readyz: can we check out (and optionally
  /// health-check) a resource within ready_wait?
  [[nodiscard]] bool ready() const {
    auto lease = checkout(Deadline{Clock::now() + opts_.ready_wait, {}});
    if (!lease) return false;
    return !opts_.health || opts_.health(**lease);
  }

  [[nodiscard]] std::size_t size() const { return shared_->size; }
  [[nodiscard]] std::size_t available() const {
    std::lock_guard lk(shared_->mu);
    return shared_->idle.size();
  }

 private:
  std::shared_ptr<Shared> shared_;
  Options opts_;
};

}  // namespace crocket
