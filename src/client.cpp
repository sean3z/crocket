// crocket::client: outbound HTTP on h2o's h2o_httpclient, on one of crocket's
// event loops (or a loop of its own when no server is running). Retries and
// the circuit breaker are in client_policy.hpp.

#include "client_policy.hpp"
#include "crocket/fairings.hpp"
#include "crocket/log.hpp"

#include <h2o.h>
#include <h2o/hostinfo.h>
#include <h2o/httpclient.h>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore>
#include <utility>
#include <vector>

namespace crocket::http {
namespace detail {
struct ClientState;
}
namespace {

using client_detail::Circuit;
using client_detail::FailKind;
using client_detail::RetryBudget;
using client_detail::backoff_cap;
using client_detail::breaker_failure;
using client_detail::effective_timeout;
using client_detail::full_jitter;
using client_detail::header_is;
using client_detail::parse_retry_after;
using client_detail::should_retry;
using client_detail::upstream_circuit;
using client_detail::upstream_timeout;
using client_detail::upstream_unavailable;

struct Pin;

void post_async(Pin* pin, std::move_only_function<void()> fn);
void ensure_loop(detail::ClientState* impl);

std::mutex g_mu;
std::condition_variable g_idle;
std::vector<detail::ClientState*> g_clients;
Pin* g_pin = nullptr;
thread_local Pin* t_pin = nullptr;

struct Job {
  h2o_multithread_message_t msg;
  std::move_only_function<void()> run;
};

struct Pin {
  h2o_evloop_t* evloop = nullptr;
  h2o_multithread_queue_t* queue = nullptr;
  h2o_multithread_receiver_t jobs{};
  h2o_multithread_receiver_t getaddr{};
  std::thread::id tid{};
  bool server = false;
  std::thread thread;
  std::atomic<bool> stop{false};
};

void on_jobs(h2o_multithread_receiver_t*, h2o_linklist_t* messages) {
  while (!h2o_linklist_is_empty(messages)) {
    auto* job = reinterpret_cast<Job*>(messages->next);
    h2o_linklist_unlink(&job->msg.link);
    std::unique_ptr<Job> owned(job);
    owned->run();
  }
}

void post_async(Pin* pin, std::move_only_function<void()> fn) {
  auto* job = new Job{{}, std::move(fn)};
  h2o_multithread_send_message(&pin->jobs, &job->msg);
}

void ignore_sigpipe() {
  static std::once_flag once;
  std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

// A loop for calls made with no server running. Left alive for the process,
// same as the sleep_for fallback pool: destroying it races the thread that
// posts the last completion.
void start_private() {
  ignore_sigpipe();
  auto* pin = new Pin;
  pin->server = false;
  pin->evloop = h2o_evloop_create();
  pin->queue = h2o_multithread_create_queue(pin->evloop);
  std::binary_semaphore ready{0};
  pin->thread = std::thread([pin, &ready] {
    pin->tid = std::this_thread::get_id();
    t_pin = pin;
    h2o_multithread_register_receiver(pin->queue, &pin->jobs, on_jobs);
    h2o_multithread_register_receiver(pin->queue, &pin->getaddr, h2o_hostinfo_getaddr_receiver);
    ready.release();
    while (!pin->stop.load(std::memory_order_relaxed)) h2o_evloop_run(pin->evloop, 1000);
    t_pin = nullptr;
  });
  ready.acquire();
  g_pin = pin;
}

struct Slot {
  h2o_httpclient_ctx_t ctx{};
  h2o_httpclient_connection_pool_t connpool{};
  bool ready = false;
};

std::string sanitize_label(std::string_view in) {
  std::string o;
  o.reserve(in.size());
  for (char c : in) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\r') continue;
    o += c;
    if (o.size() == 200) break;
  }
  return o.empty() ? "upstream" : o;
}

std::string join_path(std::string_view prefix, std::string_view path) {
  if (path.empty()) return prefix.empty() ? std::string("/") : std::string(prefix);
  if (path.front() == '/') return std::string(path);
  if (prefix.empty() || prefix == "/") return "/" + std::string(path);
  std::string o(prefix);
  if (o.back() != '/') o += '/';
  o.append(path);
  return o;
}

bool dropped_request_header(std::string_view name) {
  return header_is(name, "host") || header_is(name, "content-length") || header_is(name, "transfer-encoding") ||
         header_is(name, "connection") || header_is(name, "keep-alive") || header_is(name, "upgrade") ||
         header_is(name, "proxy-connection") || header_is(name, "te");
}

// ---- metrics ------------------------------------------------------------------------

constexpr std::array<std::int64_t, 14> kBucketsNs = {
    500'000,      1'000'000,    2'500'000,    5'000'000,     10'000'000,    25'000'000,   50'000'000,
    100'000'000,  250'000'000,  500'000'000,  1'000'000'000, 2'500'000'000, 5'000'000'000, 10'000'000'000};

struct Counter {
  std::string upstream, method, status;
  std::atomic<std::uint64_t> n{0};
};
struct Hist {
  std::string upstream, method;
  std::array<std::atomic<std::uint64_t>, kBucketsNs.size()> buckets{};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> sum_ns{0};
};

struct MetricsReg {
  std::mutex mu;
  std::deque<std::unique_ptr<Counter>> counters;
  std::deque<std::unique_ptr<Hist>> hists;
};
MetricsReg& metrics() {
  static auto* m = new MetricsReg;
  return *m;
}

std::string prom_label(std::string_view v) {
  std::string o;
  for (char c : v) {
    if (c == '\\' || c == '"') o += '\\';
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    o += c;
  }
  return o;
}

void render_metrics(std::string& o);

void observe(const std::string& upstream, std::string_view method, std::string_view status, std::int64_t ns) {
  static std::once_flag once;
  std::call_once(once, [] { crocket::detail::add_metrics_renderer(&render_metrics); });
  auto& m = metrics();
  Counter* counter = nullptr;
  Hist* hist = nullptr;
  {
    std::lock_guard lk(m.mu);
    for (auto& c : m.counters)
      if (c->upstream == upstream && c->method == method && c->status == status) {
        counter = c.get();
        break;
      }
    if (!counter) {
      m.counters.push_back(std::make_unique<Counter>());
      counter = m.counters.back().get();
      counter->upstream = upstream;
      counter->method = std::string(method);
      counter->status = std::string(status);
    }
    for (auto& h : m.hists)
      if (h->upstream == upstream && h->method == method) {
        hist = h.get();
        break;
      }
    if (!hist) {
      m.hists.push_back(std::make_unique<Hist>());
      hist = m.hists.back().get();
      hist->upstream = upstream;
      hist->method = std::string(method);
    }
  }
  counter->n.fetch_add(1, std::memory_order_relaxed);
  if (ns < 0) ns = 0;
  std::size_t i = 0;
  while (i < kBucketsNs.size() && ns > kBucketsNs[i]) ++i;
  if (i < kBucketsNs.size()) hist->buckets[i].fetch_add(1, std::memory_order_relaxed);
  hist->sum_ns.fetch_add(std::uint64_t(ns), std::memory_order_relaxed);
  hist->count.fetch_add(1, std::memory_order_relaxed);
}

// ---- one client ----------------------------------------------------------------------

struct Outcome {
  std::expected<Response, ApiError> result = std::unexpected(upstream_unavailable("no attempt"));
  FailKind kind = FailKind::Local;
  int status = 0;
  std::optional<std::chrono::milliseconds> retry_after;
};

struct Attempt : std::enable_shared_from_this<Attempt> {
  struct TimerHook {
    h2o_timer_t timer;
    Attempt* self = nullptr;
  };

  detail::ClientState* impl = nullptr;  // the caller's shared_ptr keeps it alive across the await
  std::string url;
  http::Method method = http::Method::Get;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string request_id;
  std::string idempotency_key;
  Clock::time_point deadline{};
  std::stop_token stop;
  std::chrono::milliseconds connect_budget{0};
  std::size_t max_body = 0;

  TimerHook hook{};
  bool timer_linked = false;
  h2o_mem_pool_t* pool = nullptr;
  h2o_httpclient_t* client = nullptr;
  bool in_callback = false;
  std::atomic<bool> settled{false};
  std::atomic<bool> canceled{false};
  std::atomic<bool> suspended{false};
  Outcome outcome;
  Response response;
  std::coroutine_handle<> handle;
  crocket::detail::Exchange* exchange = nullptr;
  std::optional<std::stop_callback<std::function<void()>>> stop_cb;

  Attempt() { hook.self = this; }
  void arm_stop() {
    if (!stop.stop_possible()) return;
    // weak_ptr: the callback lives inside the attempt, so a shared_ptr here would cycle.
    stop_cb.emplace(stop, [self = weak_from_this()] {
      auto attempt = self.lock();
      if (!attempt) return;
      attempt->canceled.store(true, std::memory_order_relaxed);
      std::lock_guard lk(g_mu);
      if (!g_pin) return;
      post_async(g_pin, [attempt] {
        if (!attempt->settled.load()) attempt->finish_timeout("request canceled");
      });
    });
  }

  void finish_timeout(const char* detail) { finish(std::unexpected(upstream_timeout(detail)), FailKind::Timeout, 0, {}); }
  void finish_unavailable(std::string detail) {
    finish(std::unexpected(upstream_unavailable(std::move(detail))), FailKind::Connect, 0, {});
  }

  void finish(std::expected<Response, ApiError> result, FailKind kind, int status,
              std::optional<std::chrono::milliseconds> retry_after) {
    if (settled.exchange(true)) return;
    outcome = Outcome{std::move(result), kind, status, std::move(retry_after)};
    if (timer_linked) {
      h2o_timer_unlink(&hook.timer);
      timer_linked = false;
    }
    if (client && !in_callback) {
      auto* c = std::exchange(client, nullptr);
      if (c->cancel) c->cancel(c);
    } else {
      client = nullptr;
    }
    if (pool) {
      h2o_mem_clear_pool(pool);
      std::free(pool);
      pool = nullptr;
    }
    if (suspended.load(std::memory_order_acquire) && handle) crocket::detail::resume_on_worker(exchange, handle);
  }

  void start();
};

void on_attempt_timeout(h2o_timer_t* timer) {
  auto* attempt = reinterpret_cast<Attempt::TimerHook*>(timer)->self;
  attempt->timer_linked = false;
  if (attempt->canceled.load(std::memory_order_relaxed)) attempt->finish_timeout("request canceled");
  else attempt->finish_timeout("timeout");
}

int on_body(h2o_httpclient_t* client, const char* errstr, h2o_header_t*, size_t) {
  auto* attempt = static_cast<Attempt*>(client->data);
  attempt->in_callback = true;
  struct Reset {
    Attempt* a;
    ~Reset() { a->in_callback = false; }
  } reset{attempt};
  if (client->buf && *client->buf && (*client->buf)->size) {
    auto* buf = *client->buf;
    if (attempt->response.body.size() + buf->size > attempt->max_body) {
      attempt->finish(std::unexpected(upstream_unavailable("response too large")), FailKind::TooLarge, attempt->response.status,
                      {});
      return -1;
    }
    attempt->response.body.append(buf->bytes, buf->size);
    h2o_buffer_consume(&(*client->buf), buf->size);
    if (client->update_window) client->update_window(client);
  }
  if (errstr == nullptr) return 0;
  if (errstr != h2o_httpclient_error_is_eos) {
    attempt->client = nullptr;
    const bool timed = errstr == h2o_httpclient_error_io_timeout || errstr == h2o_httpclient_error_first_byte_timeout ||
                       errstr == h2o_httpclient_error_connect_timeout;
    if (timed) attempt->finish_timeout(errstr);
    else attempt->finish_unavailable(errstr);
    return -1;
  }
  attempt->client = nullptr;
  int status = attempt->response.status;
  std::optional<std::chrono::milliseconds> retry_after;
  if (status == 429 || status == 503)
    if (auto v = attempt->response.headers.get("retry-after")) retry_after = parse_retry_after(*v);
  attempt->finish(std::move(attempt->response), FailKind::Http, status, std::move(retry_after));
  return 0;
}

h2o_httpclient_body_cb on_head(h2o_httpclient_t* client, const char* errstr, h2o_httpclient_on_head_t* args) {
  auto* attempt = static_cast<Attempt*>(client->data);
  attempt->in_callback = true;
  struct Reset {
    Attempt* a;
    ~Reset() { a->in_callback = false; }
  } reset{attempt};
  if (errstr != nullptr && errstr != h2o_httpclient_error_is_eos) {
    attempt->client = nullptr;
    const bool timed = errstr == h2o_httpclient_error_io_timeout || errstr == h2o_httpclient_error_first_byte_timeout ||
                       errstr == h2o_httpclient_error_connect_timeout;
    if (timed) attempt->finish_timeout(errstr);
    else attempt->finish_unavailable(errstr ? errstr : "upstream error");
    return nullptr;
  }
  attempt->response.status = args->status;
  attempt->outcome.status = args->status;
  for (std::size_t i = 0; i < args->num_headers && i < 128; ++i) {
    auto& h = args->headers[i];
    if (!h.name || !h.name->base) continue;
    std::string_view name(h.name->base, h.name->len);
    if (name.starts_with(':')) continue;
    std::string_view value(h.value.base ? h.value.base : "", h.value.len);
    attempt->response.headers.add(name, value);
  }
  std::optional<std::chrono::milliseconds> retry_after;
  if (args->status == 429 || args->status == 503)
    if (auto v = attempt->response.headers.get("retry-after")) retry_after = parse_retry_after(*v);
  if (errstr == h2o_httpclient_error_is_eos) {
    attempt->client = nullptr;
    attempt->finish(std::move(attempt->response), FailKind::Http, args->status, retry_after);
    return nullptr;
  }
  return on_body;
}

h2o_httpclient_head_cb on_connect(h2o_httpclient_t* client, const char* errstr, h2o_iovec_t* method, h2o_url_t* url,
                                  const h2o_header_t** headers, std::size_t* num_headers, h2o_iovec_t* body,
                                  h2o_httpclient_proceed_req_cb* proceed, h2o_httpclient_properties_t* props, h2o_url_t*) {
  auto* attempt = static_cast<Attempt*>(client->data);
  attempt->in_callback = true;
  struct Reset {
    Attempt* a;
    ~Reset() { a->in_callback = false; }
  } reset{attempt};
  if (errstr != nullptr) {
    attempt->client = nullptr;
    const bool timed = errstr == h2o_httpclient_error_connect_timeout || errstr == h2o_httpclient_error_io_timeout ||
                       errstr == h2o_httpclient_error_first_byte_timeout;
    if (timed) attempt->finish_timeout(errstr);
    else attempt->finish_unavailable(errstr);
    return nullptr;
  }
  auto name = http::method_name(attempt->method);
  *method = h2o_iovec_init(name.data(), name.size());
  h2o_url_t parsed{};
  if (h2o_url_parse(client->pool, attempt->url.data(), attempt->url.size(), &parsed) != 0) {
    attempt->client = nullptr;
    attempt->finish_unavailable("bad url");
    return nullptr;
  }
  *url = parsed;
  h2o_headers_t vec{};
  auto add = [&](std::string_view n, std::string_view v) {
    std::string lower(n);
    for (char& c : lower)
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    h2o_add_header_by_str(client->pool, &vec, lower.data(), lower.size(), 1, nullptr, v.data(), v.size());
  };
  bool saw_ua = false, saw_id = false, saw_key = false;
  for (auto& [n, v] : attempt->headers) {
    if (dropped_request_header(n)) continue;
    if (header_is(n, "user-agent")) saw_ua = true;
    if (header_is(n, "x-request-id")) saw_id = true;
    if (header_is(n, "idempotency-key")) saw_key = true;
    add(n, v);
  }
  if (!saw_ua) add("user-agent", "crocket");
  if (!saw_id && !attempt->request_id.empty()) add("x-request-id", attempt->request_id);
  if (!saw_key && !attempt->idempotency_key.empty()) add("idempotency-key", attempt->idempotency_key);
  bool with_body = !attempt->body.empty() || attempt->method == http::Method::Post || attempt->method == http::Method::Put ||
                   attempt->method == http::Method::Patch;
  if (with_body) {
    auto* len = static_cast<char*>(h2o_mem_alloc_pool(client->pool, char, 32));
    int n = std::snprintf(len, 32, "%zu", attempt->body.size());
    h2o_add_header(client->pool, &vec, H2O_TOKEN_CONTENT_LENGTH, nullptr, len, std::size_t(n < 0 ? 0 : n));
  }
  *headers = vec.entries;
  *num_headers = vec.size;
  *body = h2o_iovec_init(attempt->body.empty() ? nullptr : attempt->body.data(), attempt->body.size());
  *proceed = nullptr;
  if (props->connection_header) *props->connection_header = h2o_iovec_init(H2O_STRLIT("keep-alive"));
  return on_head;
}

}  // namespace

struct detail::ClientState : std::enable_shared_from_this<detail::ClientState> {
  std::string upstream;
  std::string origin;
  std::string path_prefix;
  bool https = false;
  std::chrono::milliseconds timeout{2'000};
  std::chrono::milliseconds connect_timeout{500};
  retry::Policy retry = retry::idempotent(3);
  breaker::Policy breaker_policy = breaker::consecutive(5, std::chrono::milliseconds{30'000});
  bool propagate_request_id = true;
  std::size_t max_body = std::size_t{1} << 20;
  RetryBudget budget;
  Circuit circuit;
  std::unique_ptr<h2o_socketpool_t> sockpool;
  h2o_socketpool_target_t* target = nullptr;
  Slot slot;
  bool registered = false;
  std::atomic<bool> pool_disposed{false};
  std::atomic<bool> closing{false};
  std::atomic<int> busy{0};

  static ClientOptions normalized(ClientOptions options) {
    if (options.timeout.count() < 1) options.timeout = std::chrono::milliseconds{1};
    if (options.connect_timeout.count() < 1) options.connect_timeout = std::chrono::milliseconds{1};
    if (options.connect_timeout > options.timeout) options.connect_timeout = options.timeout;
    if (options.retry.attempts < 0) options.retry = retry::idempotent(3);
    if (options.breaker.mode == breaker::Mode::Inherit)
      options.breaker = breaker::consecutive(5, std::chrono::milliseconds{30'000});
    if (options.max_body == 0) options.max_body = std::size_t{1} << 20;
    return options;
  }

  explicit ClientState(ClientOptions options) : ClientState(normalized(std::move(options)), 0) {}

  ClientState(ClientOptions options, int)
      : timeout(options.timeout),
        connect_timeout(options.connect_timeout),
        retry(options.retry),
        breaker_policy(options.breaker),
        propagate_request_id(options.propagate_request_id),
        max_body(options.max_body),
        budget(options.retry.budget, options.retry.budget_min),
        circuit(options.breaker) {
    if (options.base.empty()) throw std::invalid_argument("crocket::http::Client: base URL is empty");

    h2o_mem_pool_t pool;
    h2o_mem_init_pool(&pool);
    h2o_url_t parsed{};
    if (h2o_url_parse(&pool, options.base.data(), options.base.size(), &parsed) != 0 || parsed.scheme == nullptr ||
        (parsed.scheme != &H2O_URL_SCHEME_HTTP && parsed.scheme != &H2O_URL_SCHEME_HTTPS) || parsed.host.len == 0) {
      h2o_mem_clear_pool(&pool);
      throw std::invalid_argument("crocket::http::Client: base must be an absolute http or https URL");
    }
    https = parsed.scheme == &H2O_URL_SCHEME_HTTPS;
    path_prefix.assign(parsed.path.base, parsed.path.len);
    origin.assign(parsed.scheme->name.base, parsed.scheme->name.len);
    origin += "://";
    origin.append(parsed.authority.base, parsed.authority.len);
    if (options.name.empty()) upstream = sanitize_label(std::string_view(parsed.authority.base, parsed.authority.len));
    else upstream = sanitize_label(options.name);

    std::string origin_url = origin + "/";
    h2o_url_t origin_parsed{};
    if (h2o_url_parse(&pool, origin_url.data(), origin_url.size(), &origin_parsed) != 0) {
      h2o_mem_clear_pool(&pool);
      throw std::invalid_argument("crocket::http::Client: base URL has no host");
    }
    target = h2o_socketpool_create_target(&origin_parsed, nullptr);
    auto cap = options.pool == 0 ? std::size_t{1} : std::min(options.pool, std::size_t{4096});
    sockpool = std::make_unique<h2o_socketpool_t>();
    h2o_socketpool_init_specific(sockpool.get(), cap, &target, 1, nullptr);
    h2o_socketpool_set_timeout(sockpool.get(), 10'000);
    if (https) {
      SSL_CTX* ssl = SSL_CTX_new(TLS_client_method());
      if (!ssl) {
        h2o_socketpool_dispose(sockpool.get());
        sockpool.reset();
        target = nullptr;
        h2o_mem_clear_pool(&pool);
        throw std::invalid_argument("crocket::http::Client: TLS context failed");
      }
      SSL_CTX_set_min_proto_version(ssl, TLS1_2_VERSION);
      if (!options.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ssl, options.ca_file.c_str(), nullptr) != 1) {
          SSL_CTX_free(ssl);
          h2o_socketpool_dispose(sockpool.get());
          sockpool.reset();
          target = nullptr;
          h2o_mem_clear_pool(&pool);
          throw std::invalid_argument("crocket::http::Client: could not load ca_file");
        }
      } else {
        SSL_CTX_set_default_verify_paths(ssl);
      }
      SSL_CTX_set_verify(ssl, options.verify_tls ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
      h2o_socketpool_set_ssl_ctx(sockpool.get(), ssl);
      SSL_CTX_free(ssl);
    }
    h2o_mem_clear_pool(&pool);
  }

  ~ClientState() { dispose_pool_here(); }

  int breaker_state() { return circuit.state(); }

  void enroll() {
    std::lock_guard lk(g_mu);
    g_clients.push_back(this);
  }
  void unenroll() {
    std::lock_guard lk(g_mu);
    std::erase(g_clients, this);
  }

  void dispose_pool_here() {
    if (pool_disposed.exchange(true)) return;
    if (slot.ready) close_h2();
    if (registered && sockpool && t_pin && t_pin->evloop)
      h2o_socketpool_unregister_loop(sockpool.get(), t_pin->evloop);
    registered = false;
    if (sockpool) {
      h2o_socketpool_dispose(sockpool.get());
      sockpool.reset();
      target = nullptr;
    }
  }

  void close_h2() {
    for (int i = 0; i < 1024 && !h2o_linklist_is_empty(&slot.connpool.http2.conns); ++i) {
      auto* conn = H2O_STRUCT_FROM_MEMBER(h2o_httpclient__h2_conn_t, link, slot.connpool.http2.conns.next);
      if (!conn->sock) break;
      h2o_socket_close(conn->sock);
      if (!h2o_linklist_is_empty(&slot.connpool.http2.conns) && slot.connpool.http2.conns.next == &conn->link) break;
    }
  }

  void shutdown() {
    closing.store(true);
    std::unique_lock lk(g_mu);
    g_idle.wait(lk, [&] { return busy.load() == 0; });
    Pin* pin = g_pin;
    bool on_pin = pin && pin->tid == std::this_thread::get_id();
    lk.unlock();
    if (pool_disposed.load()) return;
    if (!registered || on_pin) dispose_pool_here();
    else if (pin) {
      std::binary_semaphore done{0};
      post_async(pin, [this, &done] {
        dispose_pool_here();
        done.release();
      });
      done.acquire();
    }
  }

  void prepare_slot(Pin* pin) {
    if (slot.ready) return;
    auto ms = [](std::chrono::milliseconds d) -> std::uint64_t {
      auto n = d.count();
      return n < 1 ? 1 : std::uint64_t(n);
    };
    slot.ctx.loop = pin->evloop;
    slot.ctx.getaddr_receiver = &pin->getaddr;
    slot.ctx.io_timeout = ms(timeout);
    slot.ctx.connect_timeout = ms(connect_timeout);
    slot.ctx.first_byte_timeout = ms(timeout);
    slot.ctx.keepalive_timeout = 10'000;
    slot.ctx.max_buffer_size = 256 * 1024;
    slot.ctx.http2.max_concurrent_streams = 100;
    slot.ctx.protocol_selector.ratio.http2 = https ? -1 : 0;
    slot.ctx.protocol_selector.ratio.http3 = 0;
    h2o_httpclient_connection_pool_init(&slot.connpool, sockpool.get());
    if (!registered) {
      h2o_socketpool_register_loop(sockpool.get(), pin->evloop);
      registered = true;
    }
    slot.ready = true;
  }

  std::shared_ptr<Attempt> make_attempt(http::Method method, const std::string& url, const CallOptions& options,
                                        const std::string& request_id, Clock::time_point deadline, const std::stop_token& stop,
                                        std::chrono::milliseconds budget) {
    auto attempt = std::make_shared<Attempt>();
    detail::ClientState* self = this;
    attempt->impl = self;
    attempt->url = url;
    attempt->method = method;
    attempt->body = options.body;
    attempt->headers = options.headers;
    attempt->request_id = request_id;
    attempt->idempotency_key = options.idempotency_key;
    attempt->deadline = deadline;
    attempt->stop = stop;
    attempt->connect_budget = budget;
    attempt->max_body = max_body;
    return attempt;
  }
};

namespace {

void render_metrics(std::string& o) {
  auto& m = metrics();
  std::lock_guard lk(m.mu);
  if (!m.counters.empty()) {
    o += "# HELP crocket_client_requests_total Outbound calls by upstream, method, and outcome.\n"
         "# TYPE crocket_client_requests_total counter\n";
    for (auto& c : m.counters)
      o += "crocket_client_requests_total{upstream=\"" + prom_label(c->upstream) + "\",method=\"" + c->method +
           "\",status=\"" + prom_label(c->status) + "\"} " + std::to_string(c->n.load(std::memory_order_relaxed)) + "\n";
  }
  if (!m.hists.empty()) {
    o += "# HELP crocket_client_request_duration_seconds Outbound call duration, retries included.\n"
         "# TYPE crocket_client_request_duration_seconds histogram\n";
    for (auto& h : m.hists) {
      auto labels = "upstream=\"" + prom_label(h->upstream) + "\",method=\"" + h->method + "\"";
      std::uint64_t cum = 0;
      auto n = h->count.load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < kBucketsNs.size(); ++i) {
        cum += h->buckets[i].load(std::memory_order_relaxed);
        char le[32];
        std::snprintf(le, sizeof le, "%g", double(kBucketsNs[i]) / 1e9);
        o += "crocket_client_request_duration_seconds_bucket{" + labels + ",le=\"" + le + "\"} " +
             std::to_string(std::min(cum, n)) + "\n";
      }
      o += "crocket_client_request_duration_seconds_bucket{" + labels + ",le=\"+Inf\"} " + std::to_string(n) + "\n";
      char sum[48];
      std::snprintf(sum, sizeof sum, "%.6f", double(h->sum_ns.load(std::memory_order_relaxed)) / 1e9);
      o += "crocket_client_request_duration_seconds_sum{" + labels + "} " + sum + "\n";
      o += "crocket_client_request_duration_seconds_count{" + labels + "} " + std::to_string(n) + "\n";
    }
  }
  std::lock_guard gk(g_mu);
  if (g_clients.empty()) return;
  o += "# HELP crocket_client_breaker_state Circuit breaker: 0 closed, 1 half-open, 2 open.\n"
       "# TYPE crocket_client_breaker_state gauge\n";
  struct Row {
    std::string name;
    int state = 0;
  };
  std::vector<Row> rows;
  for (auto* client : g_clients) {
    int st = client->breaker_state();
    auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.name == client->upstream; });
    if (it == rows.end()) rows.push_back(Row{client->upstream, st});
    else it->state = std::max(it->state, st);
  }
  for (auto& row : rows)
    o += "crocket_client_breaker_state{upstream=\"" + prom_label(row.name) + "\"} " + std::to_string(row.state) + "\n";
}

void loop_enter(void* evloop, void* queue) {
  std::lock_guard lk(g_mu);
  if (g_pin) return;
  ignore_sigpipe();
  auto* pin = new Pin;
  pin->server = true;
  pin->evloop = static_cast<h2o_evloop_t*>(evloop);
  pin->queue = static_cast<h2o_multithread_queue_t*>(queue);
  pin->tid = std::this_thread::get_id();
  h2o_multithread_register_receiver(pin->queue, &pin->jobs, on_jobs);
  h2o_multithread_register_receiver(pin->queue, &pin->getaddr, h2o_hostinfo_getaddr_receiver);
  t_pin = pin;
  g_pin = pin;
}

void loop_leave(void* evloop) {
  Pin* pin = nullptr;
  std::vector<detail::ClientState*> clients;
  {
    std::lock_guard lk(g_mu);
    if (!g_pin || g_pin->evloop != evloop) return;
    pin = g_pin;
    clients = g_clients;
  }
  for (auto* client : clients) client->dispose_pool_here();
  h2o_multithread_unregister_receiver(pin->queue, &pin->jobs);
  h2o_multithread_unregister_receiver(pin->queue, &pin->getaddr);
  {
    std::lock_guard lk(g_mu);
    if (g_pin == pin) g_pin = nullptr;
  }
  t_pin = nullptr;
  delete pin;
}

void Attempt::start() {
  if (settled.load() || canceled.load()) {
    if (!settled.load()) finish_timeout(canceled.load() ? "request canceled" : "timeout");
    return;
  }
  if (!t_pin) {
    finish_unavailable("no event loop");
    return;
  }
  impl->prepare_slot(t_pin);
  pool = static_cast<h2o_mem_pool_t*>(std::malloc(sizeof(h2o_mem_pool_t)));
  if (!pool) {
    finish_unavailable("out of memory");
    return;
  }
  h2o_mem_init_pool(pool);
  void* raw_url = h2o_mem_alloc_pool_aligned(pool, alignof(h2o_url_t), sizeof(h2o_url_t));
  auto* target = static_cast<h2o_url_t*>(raw_url);
  *target = h2o_url_t{};
  if (h2o_url_parse(pool, url.data(), url.size(), target) != 0) {
    finish_unavailable("bad url");
    return;
  }
  auto ms = connect_budget.count() < 1 ? std::uint64_t{1} : std::uint64_t(connect_budget.count());
  h2o_timer_init(&hook.timer, on_attempt_timeout);
  h2o_timer_link(t_pin->evloop, ms, &hook.timer);
  timer_linked = true;
  h2o_httpclient_connect(&client, pool, this, &impl->slot.ctx, &impl->slot.connpool, target, nullptr, on_connect);
}

struct Busy {
  detail::ClientState* impl;
  explicit Busy(detail::ClientState* i) : impl(i) { impl->busy.fetch_add(1, std::memory_order_relaxed); }
  void release() {
    if (!impl) return;
    if (impl->busy.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lk(g_mu);
      g_idle.notify_all();
    }
    impl = nullptr;
  }
  ~Busy() { release(); }
};

class Perform {
 public:
  static constexpr bool crocket_resumes_on_worker = true;
  explicit Perform(std::shared_ptr<Attempt> attempt) : attempt_(std::move(attempt)) {}
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) {
    attempt_->handle = handle;
    attempt_->exchange = crocket::detail::current_exchange();
    attempt_->arm_stop();
    attempt_->suspended.store(true, std::memory_order_release);
    std::lock_guard lk(g_mu);
    if (!g_pin) {
      attempt_->finish_unavailable("no event loop");
      return false;
    }
    post_async(g_pin, [attempt = attempt_] { attempt->start(); });
    return !attempt_->settled.load();
  }
  Outcome await_resume() { return std::move(attempt_->outcome); }

 private:
  std::shared_ptr<Attempt> attempt_;
};

void ensure_loop(detail::ClientState*) {
  std::lock_guard lk(g_mu);
  if (!g_pin) start_private();
}

std::string status_label(const Outcome& outcome) {
  if (outcome.result) return std::to_string(outcome.result->status);
  auto code = outcome.result.error().code;
  if (code == "upstream.timeout") return "timeout";
  if (code == "upstream.circuit_open") return "circuit_open";
  return "error";
}

}  // namespace

detail::ClientHandle detail::make_client(ClientOptions options) {
  auto* raw = new ClientState(std::move(options));
  raw->enroll();
  return ClientHandle(std::shared_ptr<ClientState>(raw, [](ClientState* p) { delete p; }));
}

void detail::shutdown_client(ClientHandle& handle) {
  if (!handle.p) return;
  handle.p->shutdown();
  handle.p->unenroll();
}

detail::ClientHandle::~ClientHandle() = default;

Task<std::expected<Response, ApiError>> detail::client_request(const ClientHandle& handle, Method method, std::string_view path,
                                                              CallOptions options) {
  auto impl = handle.p;
  if (!impl) co_return std::unexpected(upstream_unavailable("client is empty"));
  Busy busy(impl.get());
  if (path.find("://") != std::string_view::npos)
    co_return std::unexpected(ApiError::bad_request("client.path", "path must be relative to the client base URL"));
  if (path.find_first_of(std::string_view("\r\n", 2)) != std::string_view::npos)
    co_return std::unexpected(ApiError::bad_request("client.path", "path contains a line break"));
  for (auto& [name, value] : options.headers) {
    if (!valid_header_name(name) || !valid_header_value(value))
      co_return std::unexpected(ApiError::bad_request("client.header", "invalid header"));
  }

  ensure_loop(impl.get());
  auto retry = options.retry.attempts < 0 ? impl->retry : options.retry;
  bool use_breaker = options.breaker.mode != breaker::Mode::Off &&
                     (impl->breaker_policy.mode == breaker::Mode::Consecutive || impl->breaker_policy.mode == breaker::Mode::Rate);
  auto cap = impl->timeout;
  if (options.timeout != std::chrono::milliseconds::max() && options.timeout < cap) cap = options.timeout;

  Clock::time_point deadline = Clock::time_point::max();
  std::stop_token stop;
  std::string request_id;
  if (auto* exchange = crocket::detail::current_exchange()) {
    deadline = exchange->req.deadline.at;
    stop = exchange->req.deadline.stop;
    if (impl->propagate_request_id)
      if (auto header = exchange->req.header("x-request-id"); header && *header == exchange->req.request_id)
        request_id = exchange->req.request_id;
  }
  bool idem_key = !options.idempotency_key.empty();
  if (!idem_key)
    for (auto& [name, value] : options.headers)
      if (header_is(name, "idempotency-key") && !value.empty()) idem_key = true;

  impl->budget.note_call();
  auto started = Clock::now();
  Outcome last;
  int completed = 0;
  const int max_tries = 1 + std::max(0, retry.attempts);
  std::string url = impl->origin + join_path(impl->path_prefix, path);

  while (!impl->closing.load()) {
    if (stop.stop_requested()) {
      last = Outcome{std::unexpected(upstream_timeout("request canceled")), FailKind::Timeout, 0, {}};
      break;
    }
    auto budget = effective_timeout(deadline, cap);
    if (budget.count() <= 0) {
      last = Outcome{std::unexpected(upstream_timeout(deadline == Clock::time_point::max() ? "timeout" : "deadline")),
                     FailKind::Timeout, 0, {}};
      break;
    }
    if (use_breaker && !impl->circuit.allow()) {
      last = Outcome{std::unexpected(upstream_circuit()), FailKind::Local, 0, {}};
      break;
    }
    auto attempt = impl->make_attempt(method, url, options, request_id, deadline, stop, budget);
    last = co_await Perform(std::move(attempt));
    ++completed;
    if (use_breaker) {
      if (breaker_failure(last.kind, last.status)) {
        if (impl->circuit.on_failure())
          log::warn("upstream {upstream} circuit opened", impl->upstream);
      } else if (last.kind == FailKind::Http) {
        impl->circuit.on_success();
      }
    }
    if (!should_retry(retry, method, idem_key, last.kind, last.status, completed, max_tries)) break;
    if (!impl->budget.allow_retry()) break;
    auto delay = last.retry_after ? *last.retry_after : full_jitter(backoff_cap(retry, completed - 1));
    auto left = effective_timeout(deadline, cap);
    if (delay > left) break;
    if (delay.count() > 0) co_await sleep_for(delay);
  }
  if (impl->closing.load() && last.kind == FailKind::Local && !last.result)
    last.result = std::unexpected(upstream_unavailable("client shut down"));

  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
  observe(impl->upstream, http::method_name(method), status_label(last), ns);
  if (!last.result && last.result.error().code != std::string_view("upstream.circuit_open"))
    log::warn("upstream {upstream} {method} failed ({code})", impl->upstream, http::method_name(method),
              std::string(last.result.error().code));
  busy.release();
  co_return std::move(last.result);
}

}  // namespace crocket::http

extern "C" void crocket_client_loop_enter(void* evloop, void* queue) { crocket::http::loop_enter(evloop, queue); }

extern "C" void crocket_client_loop_leave(void* evloop) { crocket::http::loop_leave(evloop); }
