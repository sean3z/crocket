#include "log_hub.hpp"

#include "crocket/app.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
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
    const char* no_colour = std::getenv("NO_COLOR");
    colour_ = isatty(STDERR_FILENO) && !(no_colour && *no_colour);
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

void Hub::write(std::string_view line) {
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
    std::lock_guard lk(sh.mu);
    // Numbered while holding the shard, so a writer that sees the count go up
    // finds the line once it takes the shard; raised before the sleeping_ check.
    Record r{queued_.fetch_add(1), line.size()};
    sh.buf.append(reinterpret_cast<const char*>(&r), sizeof r);
    sh.buf.append(line);
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

void Hub::run() {
  std::vector<std::string> batch;  // one buffer per shard that had lines
  std::string spare;               // swapped into a shard, keeping its capacity
  std::vector<std::pair<std::uint64_t, std::string_view>> order;  // (sequence, line) of one sweep
  std::uint64_t collected = 0;
  while (true) {
    {
      std::unique_lock lk(mu_);
      auto ready = [&] { return queued_.load() > collected || stop_; };
      // After a sweep that found lines, let more gather for a moment: sweeping
      // constantly takes every thread's shard lock constantly, and moves it
      // between cores. A line waits at most about a millisecond for this;
      // flush() and shutdown wake the writer at once.
      if (pace_) work_.wait_for(lk, std::chrono::milliseconds(1), [&] { return stop_ || flush_requested_; });
      flush_requested_ = false;
      if (!ready()) {
        sleeping_.store(true);  // before wait() looks at queued_ again: see write()
        work_.wait(lk, ready);
        sleeping_.store(false);
      }
      if (stop_ && queued_.load() == collected) break;
      // Every thread's lines, in one pass.
      for (auto& sh : shards_) {
        std::lock_guard slk(sh->mu);
        if (sh->buf.empty()) continue;
        spare.clear();
        spare.swap(sh->buf);  // the shard gets an empty buffer with the spare's capacity
        batch.push_back(std::move(spare));
        spare = {};
      }
    }
    std::size_t lines = 0;
    auto drops = unreported_drops_.exchange(0);
    if (drops) {
      std::string o = R"({"ts":")";
      crocket::detail::append_iso8601_now(o);
      o += R"(","level":"warn","msg":"log lines dropped: the writer fell behind","count":)" + std::to_string(drops) + "}";
      if (text_) o = local_clock() + " WARN  " + std::to_string(drops) + " log lines dropped: the writer fell behind";
      write_line(o);
    }
    // Back in the order the lines were logged, across threads.
    order.clear();
    for (auto& buf : batch) {
      for (std::size_t at = 0; at + sizeof(Record) <= buf.size();) {
        Record r;
        std::memcpy(&r, buf.data() + at, sizeof r);
        order.emplace_back(r.seq, std::string_view(buf).substr(at + sizeof r, r.size));
        at += sizeof r + r.size;
      }
    }
    if (batch.size() > 1) std::ranges::sort(order, {}, &std::pair<std::uint64_t, std::string_view>::first);
    for (auto& [seq, line] : order) write_line(line);
    lines = order.size();
    if (!opts_.sink) std::fflush(stderr);
    collected += lines;
    written_.fetch_add(lines);
    pace_ = lines > 0;
    // Keep one buffer's capacity for the next swap; the rest go.
    if (!batch.empty()) {
      spare = std::move(batch.back());
      spare.clear();
    }
    batch.clear();
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
    o = hub->colour() ? std::format("\x1b[2m{}\x1b[0m \x1b[{}m{}\x1b[0m {}", local_clock(), hue[int(level)],
                                    upper[int(level)], msg)
                      : std::format("{} {} {}", local_clock(), upper[int(level)], msg);
    if (!request_id.empty()) o += hub->colour() ? std::format(" \x1b[2m[{}]\x1b[0m", request_id) : std::format(" [{}]", request_id);
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
