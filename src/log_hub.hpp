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
  /// Whether a request this long is logged even when the sample skips it.
  [[nodiscard]] bool slow(std::chrono::steady_clock::duration took) const {
    return opts_.slow.count() > 0 && took >= opts_.slow;
  }
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

  /// Lines a thread has logged and the writer has not written yet, in a
  /// ring only that thread writes and only the writer reads: logging a line
  /// takes no lock. A line that does not fit (the ring is full, or the line is
  /// longer than it) goes to `overflow` under `mu`, and so do the lines after
  /// it until the writer has collected them, which keeps a thread's lines in order.
  struct Shard {
    static constexpr std::size_t kRing = std::size_t(1) << 18;  // bytes, a power of two
    std::unique_ptr<char[]> ring{new char[kRing]};
    alignas(64) std::atomic<std::uint64_t> tail{0};  // bytes written, by the thread
    alignas(64) std::atomic<std::uint64_t> head{0};  // bytes written out, by the writer
    std::atomic<bool> overflowing{false};
    std::mutex mu;         // overflow only
    std::string overflow;  // lines, each a Record then its bytes
    void put(const void* p, std::size_t n, std::uint64_t at) {
      auto off = std::size_t(at & (kRing - 1));
      auto first = std::min(n, kRing - off);
      std::memcpy(ring.get() + off, p, first);
      std::memcpy(ring.get(), static_cast<const char*>(p) + first, n - first);
    }
    /// Copies `n` bytes at ring position `at` (which may wrap) to `out`.
    void get(void* out, std::size_t n, std::uint64_t at) const {
      auto off = std::size_t(at & (kRing - 1));
      auto first = std::min(n, kRing - off);
      std::memcpy(out, ring.get() + off, first);
      std::memcpy(static_cast<char*>(out) + first, ring.get(), n - first);
    }
    /// The bytes at `at` as one piece, when they do not wrap.
    [[nodiscard]] const char* contiguous(std::uint64_t at, std::size_t n) const {
      auto off = std::size_t(at & (kRing - 1));
      return off + n <= kRing ? ring.get() + off : nullptr;
    }
  };
  struct Record {
    std::uint64_t seq;  // the order lines were logged in, across threads
    std::uint64_t size;  // of the line, not counting the '\n' stored after it
  };
  bool behind() const { return pending() >= opts_.buffer / 4; }  // the writer skips its pause
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
