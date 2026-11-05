#pragma once
// Retry budget, backoff, and the circuit breaker. The HTTP client uses these;
// tests include this header directly. Not a public header.

#include "crocket/client.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
#include <random>

namespace crocket::client_detail {

inline constexpr std::chrono::milliseconds kDeadlineMargin{25};

inline ApiError upstream_timeout(std::string detail) {
  return {504, "upstream.timeout", "upstream timed out", std::move(detail)};
}
inline ApiError upstream_unavailable(std::string detail) {
  return {503, "upstream.unavailable", "upstream unavailable", std::move(detail)};
}
inline ApiError upstream_circuit() {
  return {503, "upstream.circuit_open", "upstream circuit is open", {}};
}

/// The call's budget: the client's timeout, or less when the inbound request's
/// deadline is closer. `margin` is left so the handler can still write its
/// response. No inbound deadline (the far time point) uses the client timeout whole.
inline std::chrono::milliseconds effective_timeout(Clock::time_point deadline, std::chrono::milliseconds client_timeout,
                                                   std::chrono::milliseconds margin = kDeadlineMargin) {
  auto cap = client_timeout.count() < 0 ? std::chrono::milliseconds::zero() : client_timeout;
  if (deadline == Clock::time_point::max()) return cap;
  auto now = Clock::now();
  if (now >= deadline) return std::chrono::milliseconds::zero();
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (left <= margin) return std::chrono::milliseconds::zero();
  left -= margin;
  return left < cap ? left : cap;
}

inline bool is_idempotent(http::Method method) noexcept {
  switch (method) {
    case http::Method::Get:
    case http::Method::Head:
    case http::Method::Put:
    case http::Method::Delete:
    case http::Method::Options:
      return true;
    default:
      return false;
  }
}

inline bool header_is(std::string_view name, std::string_view want) noexcept {
  if (name.size() != want.size()) return false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != want[i]) return false;
  }
  return true;
}

/// `Retry-After` as a delay. Delta-seconds, or an HTTP date. Empty when the
/// value is not one of those. A date already in the past is a zero delay.
inline std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  if (value.empty()) return std::nullopt;
  if (value.front() >= '0' && value.front() <= '9') {
    std::int64_t seconds = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec != std::errc{} || ptr != value.data() + value.size() || seconds < 0) return std::nullopt;
    return std::chrono::milliseconds{seconds * 1000};
  }
  if (auto when = http::parse_date(value)) {
    auto now = std::chrono::system_clock::now();
    if (*when <= std::chrono::time_point_cast<std::chrono::seconds>(now)) return std::chrono::milliseconds::zero();
    return std::chrono::duration_cast<std::chrono::milliseconds>(*when - now);
  }
  return std::nullopt;
}

/// Upper bound of the full-jitter delay for the Nth retry (0 is the first retry).
inline std::chrono::milliseconds backoff_cap(const retry::Policy& policy, int retry_index) {
  auto cap = policy.initial.count() < 0 ? std::chrono::milliseconds::zero() : policy.initial;
  auto max_delay = policy.max_delay.count() < 0 ? std::chrono::milliseconds::zero() : policy.max_delay;
  if (cap > max_delay) cap = max_delay;
  for (int i = 0; i < retry_index; ++i) {
    if (cap >= max_delay) return max_delay;
    if (max_delay.count() > 0 && cap.count() > max_delay.count() / 2) return max_delay;
    cap *= 2;
  }
  return cap < max_delay ? cap : max_delay;
}

/// Full jitter: uniform in `[0, cap]`.
inline std::chrono::milliseconds full_jitter(std::chrono::milliseconds cap) {
  if (cap.count() <= 0) return std::chrono::milliseconds::zero();
  thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::int64_t> dist(0, cap.count());
  return std::chrono::milliseconds{dist(rng)};
}

enum class FailKind : std::uint8_t { None, Http, Timeout, Connect, TooLarge, Local };

inline bool status_retryable(int status) noexcept {
  return status == 429 || status == 502 || status == 503 || status == 504;
}

/// Connect failures, timeouts, and 500/502/503/504 trip the breaker. 429 does
/// not: the upstream is up and asking us to slow down. Neither does a body
/// that was merely too large to keep.
inline bool breaker_failure(FailKind kind, int status) noexcept {
  if (kind == FailKind::Timeout || kind == FailKind::Connect) return true;
  if (kind != FailKind::Http) return false;
  return status == 500 || status == 502 || status == 503 || status == 504;
}

/// `completed` counts attempts so far. `max_tries` is 1 + policy.attempts.
inline bool should_retry(const retry::Policy& policy, http::Method method, bool idempotency_key, FailKind kind, int status,
                         int completed, int max_tries) noexcept {
  if (policy.attempts <= 0 || completed >= max_tries) return false;
  if (policy.idempotent_only && !is_idempotent(method) && !idempotency_key) return false;
  if (kind == FailKind::Timeout || kind == FailKind::Connect) return true;
  return kind == FailKind::Http && status_retryable(status);
}

/// Retries allowed in a sliding window of recent seconds, capped at `ratio`
/// of the calls in that window, with at least `minimum` retries permitted.
class RetryBudget {
 public:
  RetryBudget(double ratio, int minimum) : ratio_(ratio < 0 ? 0 : ratio), min_(minimum < 0 ? 0 : minimum) {}

  void note_call() noexcept { slot(now_sec()).calls.fetch_add(1, std::memory_order_relaxed); }

  /// True when another retry fits. Counts it.
  bool allow_retry() noexcept {
    auto now = now_sec();
    std::uint64_t calls = 0, retries = 0;
    for (auto& s : slots_) {
      auto sec = s.sec.load(std::memory_order_relaxed);
      if (sec < 0 || now < sec || now - sec >= kWindow) continue;
      calls += s.calls.load(std::memory_order_relaxed);
      retries += s.retries.load(std::memory_order_relaxed);
    }
    auto from_ratio = static_cast<std::uint64_t>(ratio_ * static_cast<double>(calls));
    auto limit = std::max<std::uint64_t>(static_cast<std::uint64_t>(min_), from_ratio);
    if (retries >= limit) return false;
    slot(now).retries.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

 private:
  static constexpr int kWindow = 10;
  struct Slot {
    std::atomic<std::int64_t> sec{-1};
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> retries{0};
  };
  Slot& slot(std::int64_t sec) noexcept {
    auto& s = slots_[static_cast<std::size_t>(sec % kWindow)];
    auto cur = s.sec.load(std::memory_order_relaxed);
    if (cur != sec && s.sec.compare_exchange_strong(cur, sec, std::memory_order_relaxed)) {
      s.calls.store(0, std::memory_order_relaxed);
      s.retries.store(0, std::memory_order_relaxed);
    }
    return s;
  }
  static std::int64_t now_sec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
  }
  double ratio_;
  int min_;
  Slot slots_[kWindow]{};
};

/// Per-upstream circuit. `state()` is 0 closed, 1 half-open, 2 open.
class Circuit {
 public:
  static constexpr int Closed = 0;
  static constexpr int HalfOpen = 1;
  static constexpr int Open = 2;
  static constexpr int kMaxWindow = 64;

  explicit Circuit(breaker::Policy policy) : policy_(policy) {
    if (policy_.window < 1) policy_.window = 1;
    if (policy_.window > kMaxWindow) policy_.window = kMaxWindow;
    if (policy_.half_open_probes < 1) policy_.half_open_probes = 1;
    if (policy_.backoff < 2) policy_.backoff = 2;
    if (policy_.min_samples < 1) policy_.min_samples = 1;
    if (policy_.max_open < policy_.open_for) policy_.max_open = policy_.open_for;
  }

  [[nodiscard]] int state(Clock::time_point now = Clock::now()) {
    std::lock_guard lk(mu_);
    transition(now);
    return state_;
  }

  /// False when the breaker is open (or the half-open probe slots are taken).
  [[nodiscard]] bool allow(Clock::time_point now = Clock::now()) {
    std::lock_guard lk(mu_);
    if (!enabled()) return true;
    transition(now);
    if (state_ == Closed) return true;
    if (state_ == Open) return false;
    if (probes_ >= policy_.half_open_probes) return false;
    ++probes_;
    return true;
  }

  /// True when this success closed a half-open breaker.
  bool on_success(Clock::time_point now = Clock::now()) {
    std::lock_guard lk(mu_);
    if (!enabled()) return false;
    transition(now);
    consecutive_ = 0;
    record(true);
    if (state_ != HalfOpen) return false;
    state_ = Closed;
    streak_ = 0;
    probes_ = 0;
    return true;
  }

  /// True when this failure opened the breaker (or re-opened it from half-open).
  bool on_failure(Clock::time_point now = Clock::now()) {
    std::lock_guard lk(mu_);
    if (!enabled()) return false;
    transition(now);
    record(false);
    if (state_ == HalfOpen) {
      trip(now);
      return true;
    }
    if (state_ == Open) return false;
    if (policy_.mode == breaker::Mode::Consecutive) {
      if (policy_.consecutive > 0 && ++consecutive_ >= policy_.consecutive) {
        trip(now);
        return true;
      }
      return false;
    }
    if (policy_.mode == breaker::Mode::Rate && samples_ >= policy_.min_samples && policy_.failure_rate > 0 &&
        double(failures_) >= policy_.failure_rate * double(samples_)) {
      trip(now);
      return true;
    }
    return false;
  }

 private:
  [[nodiscard]] bool enabled() const {
    return policy_.mode == breaker::Mode::Consecutive || policy_.mode == breaker::Mode::Rate;
  }
  void transition(Clock::time_point now) {
    if (state_ == Open && now >= open_until_) {
      state_ = HalfOpen;
      probes_ = 0;
    }
  }
  void trip(Clock::time_point now) {
    auto dur = policy_.open_for.count() < 0 ? std::chrono::milliseconds::zero() : policy_.open_for;
    for (int i = 0; i < streak_; ++i) {
      if (dur >= policy_.max_open) {
        dur = policy_.max_open;
        break;
      }
      if (policy_.max_open.count() > 0 && dur.count() > policy_.max_open.count() / policy_.backoff) {
        dur = policy_.max_open;
        break;
      }
      dur *= policy_.backoff;
    }
    if (dur > policy_.max_open) dur = policy_.max_open;
    ++streak_;
    open_until_ = now + dur;
    state_ = Open;
    probes_ = 0;
  }
  void record(bool ok) {
    if (policy_.mode != breaker::Mode::Rate) return;
    int window = policy_.window;
    int index = seq_ % window;
    if (seq_ >= window && !ring_[index]) --failures_;
    ring_[index] = ok;
    if (!ok) ++failures_;
    ++seq_;
    samples_ = seq_ < window ? seq_ : window;
  }

  breaker::Policy policy_;
  std::mutex mu_;
  int state_ = Closed;
  int consecutive_ = 0;
  int probes_ = 0;
  int streak_ = 0;
  int seq_ = 0;
  int samples_ = 0;
  int failures_ = 0;
  bool ring_[kMaxWindow]{};
  Clock::time_point open_until_{};
};

}  // namespace crocket::client_detail
