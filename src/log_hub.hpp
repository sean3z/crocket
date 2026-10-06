#pragma once
// The machinery behind crocket/log.hpp: a bounded queue drained by one writer
// thread per app, and the record of which request a thread is serving.

#include "crocket/log.hpp"
#include "crocket/request.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace crocket::log::detail {

class Hub {
 public:
  /// `text`: readable lines (dev profile) instead of JSON. `dropped` counts
  /// discarded lines for Metrics.
  Hub(Options opts, bool text, std::atomic<std::uint64_t>* dropped);
  ~Hub();
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  [[nodiscard]] bool enabled(Level l) const { return l >= opts_.level; }
  [[nodiscard]] bool text() const { return text_; }
  [[nodiscard]] bool colour() const { return colour_; }
  /// Whether the request line for a successful response is kept (Options::sample).
  [[nodiscard]] bool sampled(std::string_view request_id) const;
  [[nodiscard]] bool redacted(std::string_view field) const;
  /// `json` with the values of redacted members (at any depth) replaced.
  [[nodiscard]] std::string redact(std::string json) const;

  /// Queues a finished line for the writer thread.
  void write(std::string line);
  /// Returns once every line queued so far has reached the sink.
  void flush();

 private:
  void run();

  Options opts_;
  bool text_;
  bool colour_ = false;
  std::atomic<std::uint64_t>* dropped_;
  std::vector<std::string> redact_;  // lower case

  std::mutex mu_;
  std::condition_variable work_, room_, done_;
  std::deque<std::string> queue_;
  std::uint64_t queued_ = 0, written_ = 0, unreported_drops_ = 0;
  bool stop_ = false;
  std::thread writer_;  // started by the first line
};

/// The thread's request while crocket runs its handler and fairings.
class RequestScope {
 public:
  RequestScope(const std::shared_ptr<Hub>& hub, const Request* rq);
  ~RequestScope();
  RequestScope(const RequestScope&) = delete;
  RequestScope& operator=(const RequestScope&) = delete;

 private:
  const std::shared_ptr<Hub>* prev_hub_;
  const Request* prev_rq_;
  const Context* prev_ctx_;
};

/// The hub for lines logged outside any request: the last app to ignite.
void set_default(std::shared_ptr<Hub> hub);
void clear_default(const Hub* hub);

/// "14:03:07.250", local time, for text lines.
std::string local_clock();

}  // namespace crocket::log::detail
