#pragma once
// The machinery behind crocket/log.hpp: a bounded queue drained by one writer
// thread per app, and the record of which request a thread is serving.

#include "crocket/log.hpp"
#include "crocket/request.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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
  [[nodiscard]] bool color() const { return color_; }
  /// Whether the request line for a successful response is kept (Options::sample).
  [[nodiscard]] bool sampled(std::string_view request_id) const;
  [[nodiscard]] bool redacted(std::string_view field) const;
  /// `json` with the values of redacted members (at any depth) replaced.
  [[nodiscard]] std::string redact(std::string json) const;

  /// Copies a finished line into this thread's shard for the writer thread.
  void write(std::string_view line);
  /// Returns once every line queued so far has reached the sink.
  void flush();

 private:
  void run();
  void write_line(std::string_view line);

  Options opts_;
  bool text_;
  bool color_ = false;
  std::atomic<std::uint64_t>* dropped_;
  std::vector<std::string> redact_;  // lower case

  /// Lines a thread has logged and the writer has not collected yet, in a
  /// ring only that thread writes and only the writer reads: logging a line
  /// takes no lock. A line that does not fit (the ring is full, or the line is
  /// longer than it) goes to `overflow` under `mu`, and so do the lines after
  /// it until the writer has collected them, which keeps a thread's lines in order.
  struct Shard {
    static constexpr std::size_t kRing = std::size_t(1) << 18;  // bytes, a power of two
    std::unique_ptr<char[]> ring{new char[kRing]};
    alignas(64) std::atomic<std::uint64_t> tail{0};  // bytes written, by the thread
    alignas(64) std::atomic<std::uint64_t> head{0};  // bytes collected, by the writer
    std::atomic<bool> overflowing{false};
    std::mutex mu;         // overflow only
    std::string overflow;  // lines, each a Record then its bytes
    void put(const void* p, std::size_t n, std::uint64_t at) {
      auto off = std::size_t(at & (kRing - 1));
      auto first = std::min(n, kRing - off);
      std::memcpy(ring.get() + off, p, first);
      std::memcpy(ring.get(), static_cast<const char*>(p) + first, n - first);
    }
    void take(std::string& out, std::uint64_t from, std::uint64_t to) const {
      auto off = std::size_t(from & (kRing - 1));
      auto n = std::size_t(to - from);
      auto first = std::min(n, kRing - off);
      out.append(ring.get() + off, first);
      out.append(ring.get(), n - first);
    }
  };
  struct Record {
    std::uint64_t seq;  // the order lines were logged in, across threads
    std::uint64_t size;
  };
  Shard& shard();  // this thread's
  void start_writer();
  [[nodiscard]] std::uint64_t pending() const { return queued_.load() - written_.load(); }

  const std::uint64_t id_;  // never reused, unlike an address (per-thread shard caches)
  std::mutex mu_;           // the shard list, the writer's sleep, and the waits below
  std::condition_variable work_, room_, done_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> queued_{0}, written_{0}, unreported_drops_{0};
  std::atomic<bool> sleeping_{false};  // the writer waits on work_
  std::atomic<bool> started_{false};
  bool stop_ = false;
  bool flush_requested_ = false;  // guarded by mu_: skip the writer's pause
  bool pace_ = false;             // writer thread only: its last sweep found lines
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
