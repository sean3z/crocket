#include "log_hub.hpp"

#include "crocket/app.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <climits>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <format>

namespace crocket::log {
namespace detail {
namespace {

/// The request (or carried Context) this thread is logging for.
struct Current {
  const std::shared_ptr<Hub>* hub = nullptr;  // the app's, alive for the request
  const Request* rq = nullptr;
  const Context* ctx = nullptr;
};
thread_local Current t_current;

std::atomic<std::shared_ptr<Hub>> g_default;
thread_local std::shared_ptr<Hub> t_default_hold;  // keeps the default hub alive while this thread uses it

Hub& fallback() {
  static Hub hub{Options{}, false, nullptr};
  return hub;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

constexpr std::string_view level_name(Level l) {
  constexpr std::string_view names[] = {"debug", "info", "warn", "error"};
  return names[int(l)];
}

void field(std::string& o, std::string_view k, std::string_view v) {
  o += ',';
  json::write_string(o, k);
  o += ':';
  json::write_string(o, v);
}

json::Value redact_value(const Hub& hub, const json::Value& v) {
  if (v.is_array()) {
    json::Array out;
    for (auto& e : v.as_array()) out.push_back(redact_value(hub, e));
    return out;
  }
  if (v.is_object()) {
    json::Object out;
    for (auto& [k, e] : v.as_object())
      out.emplace_back(k, hub.redacted(k) ? json::Value(std::string("[redacted]")) : redact_value(hub, e));
    return out;
  }
  return v;
}

}  // namespace

// ---- Hub -------------------------------------------------------------------------

std::atomic<std::uint64_t> g_next_hub_id{1};

Hub::Hub(Options opts, bool text, std::atomic<std::uint64_t>* dropped)
    : opts_(std::move(opts)), text_(text), dropped_(dropped), id_(g_next_hub_id.fetch_add(1)) {
  for (auto& r : opts_.redact) redact_.push_back(lower(r));
  if (opts_.buffer == 0) opts_.buffer = 1;
  if (text_ && !opts_.sink) {
    const char* no_color = std::getenv("NO_COLOR");
    color_ = isatty(STDERR_FILENO) && !(no_color && *no_color);
  }
}

Hub::~Hub() {
  {
    std::lock_guard lk(mu_);
    stop_ = true;
  }
  work_.notify_all();
  if (writer_.joinable()) writer_.join();  // the writer drains the queue before it stops
}

bool Hub::sampled(std::string_view request_id) const {
  if (opts_.sample >= 1) return true;
  if (opts_.sample <= 0) return false;
  std::uint64_t h = 14695981039346656037ULL;  // FNV-1a: the same id gives the same answer everywhere
  for (unsigned char c : request_id) h = (h ^ c) * 1099511628211ULL;
  return double(h % 1'000'000) < opts_.sample * 1'000'000;
}

bool Hub::redacted(std::string_view field) const {
  if (redact_.empty()) return false;
  auto name = lower(field);
  for (auto& r : redact_)
    if (!r.empty() && name.contains(r)) return true;
  return false;
}

std::string Hub::redact(std::string json) const {
  if (redact_.empty() || json.empty() || (json[0] != '{' && json[0] != '[')) return json;
  auto v = json::parse(json, {.max_depth = 256, .max_object_members = SIZE_MAX, .max_array_elements = SIZE_MAX});
  if (!v) return json;
  std::string out;
  json::dump(redact_value(*this, *v), out);
  return out;
}

Hub::Shard& Hub::shard() {
  thread_local std::vector<std::pair<std::uint64_t, Shard*>> mine;  // hub id -> this thread's shard
  for (auto& [hub, sh] : mine)
    if (hub == id_) return *sh;
  std::lock_guard lk(mu_);
  shards_.push_back(std::make_unique<Shard>());
  if (mine.size() >= 16) mine.clear();  // entries of hubs that are gone
  mine.emplace_back(id_, shards_.back().get());
  return *shards_.back();
}

void Hub::start_writer() {
  std::lock_guard lk(mu_);
  if (started_.load()) return;
  writer_ = std::thread([this] { run(); });
  started_.store(true);
}

namespace {
void replace_all(std::string& s, std::string_view needle) {
  if (needle.size() < 4) return;
  constexpr std::string_view repl = "[redacted]";
  for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos;) {
    s.replace(at, needle.size(), repl);
    at += repl.size();
  }
}
}  // namespace

std::string without_credentials(const Request* rq, std::string text) {
  if (!rq) return text;
  auto strip = [&](std::string_view name) {
    if (auto v = rq->header(name)) replace_all(text, *v);
  };
  // The whole header value, so "Bearer <jwt>" cannot appear. The bare token is
  // removed only when it is long: a short opaque token is often also the
  // subject ("alice"), and that subject is meant to be in the log line.
  strip("authorization");
  strip("proxy-authorization");
  strip("cookie");
  if (auto h = rq->header("authorization")) {
    constexpr std::string_view scheme = "bearer ";
    if (h->size() > scheme.size() + 16 && Headers::iequals(h->substr(0, scheme.size()), scheme))
      replace_all(text, h->substr(scheme.size()));
  }
  return text;
}

void Hub::write(std::string_view line) {
  std::string scrubbed;
  if (t_current.rq && (t_current.rq->header("authorization") || t_current.rq->header("proxy-authorization") ||
                       t_current.rq->header("cookie"))) {
    scrubbed = without_credentials(t_current.rq, std::string(line));
    line = scrubbed;
  }
  if (pending() >= opts_.buffer) {
    if (opts_.when_full == WhenFull::drop) {
      unreported_drops_.fetch_add(1);
      if (dropped_) dropped_->fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::unique_lock lk(mu_);
    room_.wait(lk, [&] { return pending() < opts_.buffer || stop_; });
  }
  {
    auto& sh = shard();
    // Numbered before it is published, so a writer that sees the count go up
    // finds the line once this thread publishes it (it sweeps until it has).
    Record r{queued_.fetch_add(1), line.size()};
    auto need = sizeof r + line.size() + 1;  // the '\n' too: the writer hands lines to writev in place
    auto tail = sh.tail.load(std::memory_order_relaxed);
    if (!sh.overflowing.load(std::memory_order_acquire) &&
        need <= Shard::kRing - (tail - sh.head.load(std::memory_order_acquire))) {
      sh.put(&r, sizeof r, tail);
      sh.put(line.data(), line.size(), tail + sizeof r);
      sh.put("\n", 1, tail + sizeof r + line.size());
      sh.tail.store(tail + need, std::memory_order_release);
    } else {
      std::lock_guard lk(sh.mu);
      sh.overflow.append(reinterpret_cast<const char*>(&r), sizeof r);
      sh.overflow.append(line);
      sh.overflow += '\n';
      sh.overflowing.store(true, std::memory_order_release);
    }
  }
  if (!started_.load()) start_writer();
  // Waking the writer is a system call: only when it sleeps, which it does
  // between bursts, not per line. (It sets sleeping_ before it last looks at
  // queued_, and this looks at sleeping_ after raising queued_, so one of the
  // two always sees the other.)
  if (sleeping_.load()) {
    std::lock_guard lk(mu_);
    work_.notify_one();
  }
}

void Hub::flush() {
  auto target = queued_.load();
  std::unique_lock lk(mu_);
  flush_requested_ = true;
  work_.notify_one();
  done_.wait(lk, [&] { return written_.load() >= target || !started_.load(); });
}

namespace {

/// Writes every byte of `iov` to `fd`, retrying partial writes and EINTR.
/// False on any other error (the lines are lost, as with fwrite before).
bool write_all(int fd, std::vector<iovec>& iov) {
  std::size_t i = 0;
  while (i < iov.size()) {
    int n = int(std::min<std::size_t>(iov.size() - i, IOV_MAX));
    ssize_t w = ::writev(fd, iov.data() + i, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {  // a non-blocking descriptor: wait until it takes more
        pollfd p{fd, POLLOUT, 0};
        ::poll(&p, 1, 100);
        continue;
      }
      return false;
    }
    auto left = std::size_t(w);
    while (i < iov.size() && left >= iov[i].iov_len) left -= iov[i++].iov_len;
    if (left) {
      iov[i].iov_base = static_cast<char*>(iov[i].iov_base) + left;
      iov[i].iov_len -= left;
    }
  }
  return true;
}

/// A container runtime reads a pipe in turns: a bigger pipe rides out its
/// pauses without the writer blocking. A hint; failures are ignored.
void widen_pipe(int fd) {
#ifdef F_SETPIPE_SZ
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) return;
  int now = ::fcntl(fd, F_GETPIPE_SZ);
  if (now >= 0 && now < (1 << 20)) (void)::fcntl(fd, F_SETPIPE_SZ, 1 << 20);
#else
  (void)fd;
#endif
}

}  // namespace

void Hub::run() {
  // One sweep: what every thread has logged, put back in the order it was
  // logged in, and written with one writev per IOV_MAX lines. Lines are
  // written from where they lie (the thread's ring, or an overflow buffer)
  // with the '\n' stored after each, so the writer copies nothing on the
  // common path; a ring is released only once its lines are written.
  struct Item {
    std::uint64_t seq;
    std::string_view line;  // followed in memory by its '\n'
  };
  struct Taken {
    Shard* shard;
    std::uint64_t tail;  // the ring's new head once written
  };
  std::vector<Taken> taken;
  // Deques: adding one never moves the others, which `order` points into.
  std::deque<std::string> overflows;  // swapped out of shards, keeping their lines alive
  std::deque<std::string> wrapped;    // lines that wrapped around a ring's end, copied
  std::vector<Item> order, waiting;
  std::vector<std::pair<std::uint64_t, std::string>> held, held_next;  // lines after a gap in the numbering
  std::vector<iovec> iov;
  std::string drop_line;
  std::uint64_t collected = 0;
  if (!opts_.sink) widen_pipe(STDERR_FILENO);
  while (true) {
    taken.clear();
    {
      std::unique_lock lk(mu_);
      auto ready = [&] { return queued_.load() > collected || stop_; };
      // After a sweep that found lines, let more gather for a moment, unless
      // the writer is behind: a line waits at most about a millisecond for
      // this; flush() and shutdown wake the writer at once.
      if (pace_ && !behind())
        work_.wait_for(lk, std::chrono::milliseconds(1), [&] { return stop_ || flush_requested_; });
      flush_requested_ = false;
      if (!ready()) {
        sleeping_.store(true);  // before wait() looks at queued_ again: see write()
        work_.wait(lk, ready);
        sleeping_.store(false);
      }
      if (stop_ && queued_.load() == collected) break;
      // Every thread's lines, in one pass: its ring, then what overflowed
      // (lines logged after the ring's, and before any the ring takes next).
      for (auto& sh : shards_) {
        // `overflowing` first: ring lines published before it was set are then
        // all below the tail read next, so none is collected after a later line.
        bool over = sh->overflowing.load(std::memory_order_acquire);
        auto tail = sh->tail.load(std::memory_order_acquire);
        auto head = sh->head.load(std::memory_order_relaxed);
        if (tail != head) {
          for (auto at = head; at < tail;) {
            Record r;
            sh->get(&r, sizeof r, at);
            auto pos = at + sizeof r;
            auto n = std::size_t(r.size);
            if (const char* p = sh->contiguous(pos, n + 1)) {
              order.push_back({r.seq, std::string_view(p, n)});
            } else {
              std::string copy(n + 1, '\n');
              sh->get(copy.data(), n + 1, pos);
              wrapped.push_back(std::move(copy));
              order.push_back({r.seq, std::string_view(wrapped.back()).substr(0, n)});
            }
            at = pos + n + 1;
          }
          taken.push_back({sh.get(), tail});
        }
        if (over) {
          std::string buf;
          {
            std::lock_guard slk(sh->mu);
            buf.swap(sh->overflow);
            sh->overflowing.store(false, std::memory_order_release);
          }
          overflows.push_back(std::move(buf));
        }
      }
    }
    for (auto& buf : overflows)
      for (std::size_t at = 0; at + sizeof(Record) <= buf.size();) {
        Record r;
        std::memcpy(&r, buf.data() + at, sizeof r);
        order.push_back({r.seq, std::string_view(buf).substr(at + sizeof r, r.size)});
        at += sizeof r + r.size + 1;
      }
    for (auto& [seq, line] : held) order.push_back({seq, std::string_view(line).substr(0, line.size() - 1)});
    std::ranges::sort(order, {}, &Item::seq);
    // A line is numbered just before its thread publishes it: one numbered
    // earlier may still be on its way. Write only up to the first gap; the
    // rest waits for the next sweep (copied: its ring is released below), so
    // lines never come out of order.
    std::size_t ready_lines = 0;
    while (ready_lines < order.size() && order[ready_lines].seq == collected + ready_lines) ++ready_lines;
    for (std::size_t i = ready_lines; i < order.size(); ++i) {
      std::string copy(order[i].line);
      copy += '\n';
      held_next.emplace_back(order[i].seq, std::move(copy));
    }
    order.resize(ready_lines);

    auto drops = unreported_drops_.exchange(0);
    drop_line.clear();
    if (drops) {
      drop_line = R"({"ts":")";
      crocket::detail::append_iso8601_now(drop_line);
      drop_line += R"(","level":"warn","msg":"log lines dropped: the writer fell behind","count":)" + std::to_string(drops) + "}";
      if (text_) drop_line = local_clock() + " WARN  " + std::to_string(drops) + " log lines dropped: the writer fell behind";
    }
    if (opts_.sink) {
      if (!drop_line.empty()) write_line(drop_line);
      for (auto& it : order) write_line(it.line);
    } else if (!order.empty() || !drop_line.empty()) {
      iov.clear();
      if (!drop_line.empty()) {
        drop_line += '\n';
        iov.push_back({drop_line.data(), drop_line.size()});
      }
      for (auto& it : order) iov.push_back({const_cast<char*>(it.line.data()), it.line.size() + 1});
      std::fflush(stderr);  // anything printed with stdio first stays first
      (void)write_all(STDERR_FILENO, iov);
    }
    // The rings' lines are out (or copied into held_next): release them.
    for (auto& t : taken) t.shard->head.store(t.tail, std::memory_order_release);
    std::size_t lines = order.size();
    collected += lines;
    written_.fetch_add(lines);
    pace_ = lines > 0;
    order.clear();
    held.swap(held_next);
    held_next.clear();
    wrapped.clear();
    overflows.clear();
    std::lock_guard lk(mu_);
    room_.notify_all();
    done_.notify_all();
  }
  std::lock_guard lk(mu_);
  done_.notify_all();
}

void Hub::write_line(std::string_view line) {
  if (opts_.sink) {
    try {
      opts_.sink(line);
    } catch (...) {  // a throwing sink loses its line, not the writer
    }
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
  }
}

// ---- context ---------------------------------------------------------------------

RequestScope::RequestScope(const std::shared_ptr<Hub>& hub, const Request* rq)
    : prev_hub_(t_current.hub), prev_rq_(t_current.rq), prev_ctx_(t_current.ctx) {
  t_current = {&hub, rq, nullptr};
}
RequestScope::~RequestScope() { t_current = {prev_hub_, prev_rq_, prev_ctx_}; }

void set_default(std::shared_ptr<Hub> hub) { g_default.store(std::move(hub)); }
void clear_default(const Hub* hub) {
  auto cur = g_default.load();
  if (cur.get() == hub) g_default.compare_exchange_strong(cur, nullptr);
}

std::string local_clock() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  return buf;
}

Hub* hub_for(Level level) {
  Hub* hub = t_current.ctx ? t_current.ctx->hub_.get() : t_current.hub ? t_current.hub->get() : nullptr;
  if (!hub) {
    t_default_hold = g_default.load();
    hub = t_default_hold ? t_default_hold.get() : &fallback();
  }
  return hub->enabled(level) ? hub : nullptr;
}

void emit(Hub* hub, Level level, std::string_view text, std::span<Field> fields) {
  // Redact first: a redacted value must not reach the message either.
  for (auto& f : fields) {
    if (hub->redacted(f.name)) {
      f.json = R"("[redacted]")";
      f.text = "[redacted]";
    } else if (!f.text) {
      f.json = hub->redact(std::move(f.json));
    }
  }
  std::string msg;
  std::size_t k = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {  // "{{" and "}}"
      msg += c;
      ++i;
    } else if (c == '{') {
      auto& f = fields[k++];
      msg += f.text ? *f.text : std::string_view(f.json);
      i = text.find('}', i);
    } else {
      msg += c;
    }
  }

  // The request's context: a carried Context, or the request being handled.
  std::string_view request_id, trace_id, route, handler, subject;
  if (auto* ctx = t_current.ctx) {
    request_id = ctx->request_id_, trace_id = ctx->trace_id_, route = ctx->route_, handler = ctx->handler_,
    subject = ctx->subject_;
  } else if (auto* rq = t_current.rq) {
    request_id = rq->request_id, trace_id = rq->trace_id, route = rq->route_template, handler = rq->handler,
    subject = rq->subject;
  }

  std::string o;
  if (hub->text()) {
    static constexpr std::string_view upper[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    static constexpr std::string_view hue[] = {"2", "32", "33", "31"};
    o = hub->color() ? std::format("\x1b[2m{}\x1b[0m \x1b[{}m{}\x1b[0m {}", local_clock(), hue[int(level)],
                                    upper[int(level)], msg)
                      : std::format("{} {} {}", local_clock(), upper[int(level)], msg);
    if (!request_id.empty()) o += hub->color() ? std::format(" \x1b[2m[{}]\x1b[0m", request_id) : std::format(" [{}]", request_id);
  } else {
    o = R"({"ts":")";
    crocket::detail::append_iso8601_now(o);
    o += '"';
    field(o, "level", level_name(level));
    field(o, "msg", msg);
    if (!request_id.empty()) field(o, "request_id", request_id);
    if (!trace_id.empty()) field(o, "trace_id", trace_id);
    if (!route.empty()) field(o, "route", route);
    if (!handler.empty()) field(o, "handler", handler);
    if (!subject.empty()) field(o, "subject", subject);
    for (auto& f : fields) {
      o += ',';
      json::write_string(o, f.name);
      o += ':';
      o += f.json;
    }
    o += '}';
  }
  hub->write(std::move(o));
}

}  // namespace detail

std::string without_credentials(const Request* rq, std::string text) {
  return detail::without_credentials(rq, text);
}

Context context() {
  Context c;
  auto& cur = detail::t_current;
  if (cur.ctx) return *cur.ctx;
  if (!cur.hub || !cur.rq) return c;
  c.hub_ = *cur.hub;  // shares ownership: the context may outlive the request, and the app
  c.request_id_ = cur.rq->request_id;
  c.trace_id_ = cur.rq->trace_id;
  c.route_ = cur.rq->route_template;
  c.handler_ = cur.rq->handler;
  c.subject_ = cur.rq->subject;
  return c;
}

Scope::Scope(const Context& ctx)
    : prev_ctx_(detail::t_current.ctx), prev_rq_(detail::t_current.rq), prev_hub_(detail::t_current.hub) {
  detail::t_current = {nullptr, nullptr, &ctx};
}
Scope::~Scope() { detail::t_current = {prev_hub_, prev_rq_, prev_ctx_}; }

}  // namespace crocket::log
