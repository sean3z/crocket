#include "log_hub.hpp"

#include "crocket/app.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
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

Hub::Hub(Options opts, bool text, std::atomic<std::uint64_t>* dropped)
    : opts_(std::move(opts)), text_(text), dropped_(dropped) {
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

void Hub::write(std::string line) {
  std::unique_lock lk(mu_);
  if (queue_.size() >= opts_.buffer) {
    if (opts_.when_full == WhenFull::drop) {
      ++unreported_drops_;
      if (dropped_) dropped_->fetch_add(1, std::memory_order_relaxed);
      return;
    }
    room_.wait(lk, [&] { return queue_.size() < opts_.buffer || stop_; });
  }
  queue_.push_back(std::move(line));
  ++queued_;
  if (!writer_.joinable()) writer_ = std::thread([this] { run(); });
  lk.unlock();
  work_.notify_one();
}

void Hub::flush() {
  std::unique_lock lk(mu_);
  auto target = queued_;
  done_.wait(lk, [&] { return written_ >= target || !writer_.joinable(); });
}

void Hub::run() {
  std::deque<std::string> batch;
  std::unique_lock lk(mu_);
  while (true) {
    work_.wait(lk, [&] { return !queue_.empty() || stop_; });
    if (queue_.empty() && stop_) break;
    batch.swap(queue_);
    auto drops = std::exchange(unreported_drops_, 0);
    lk.unlock();
    room_.notify_all();
    if (drops) {
      std::string o = R"({"ts":)";
      json::write_string(o, crocket::detail::iso8601_now());
      o += R"(,"level":"warn","msg":"log lines dropped: the writer fell behind","count":)" + std::to_string(drops) + "}";
      if (text_) o = local_clock() + " WARN  " + std::to_string(drops) + " log lines dropped: the writer fell behind";
      batch.push_front(std::move(o));
    }
    for (auto& line : batch) {
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
    if (!opts_.sink) std::fflush(stderr);
    std::size_t n = batch.size() - (drops ? 1 : 0);
    batch.clear();
    lk.lock();
    written_ += n;
    done_.notify_all();
  }
  done_.notify_all();
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
    o = R"({"ts":)";
    json::write_string(o, crocket::detail::iso8601_now());
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
