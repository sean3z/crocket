// The wire engine: h2o (libh2o-evloop) on one event-loop thread, handlers on a
// worker pool. This is the only translation unit that includes h2o.
//
// Flow per HTTP transaction (an h1 request or an h2 stream; h2o gives each its
// own h2o_req_t):
//
//   on_req              build Request from h2o's parsed request, arm the deadline,
//                       reject early (431 / 413 / 400), else take the body
//   on_body             (write_req.cb) append, enforce max_body_bytes, ask h2o for
//                       more with proceed_req; dispatch at the end of the body
//   (worker)            Crocket::handle -> h2o_multithread message
//   on_completions      write status, headers, body, and trailers over h2
//   on_deadline         deadline hit: cancel token, answer 504
//   dispose_session     h2o freed the request (response sent or client gone):
//                       cancel token, forget the session
//
// See docs/ENGINE.md.

#include "engine.hpp"
#include "crocket/grpc.hpp"

#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>
#include <h2o/httpclient.h>

#include <netdb.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace crocket::detail {
namespace {

using namespace std::chrono_literals;

/// After answering early (413, 504, ...) an h1 connection must still read the
/// rest of the body before it can take the next request. Up to this much is
/// read and dropped; past it the connection is closed after the response.
constexpr std::size_t kMaxDiscard = 8 << 20;

std::atomic<bool> g_stop{false};
std::atomic<int> g_wake_fd{-1};

extern "C" void on_signal(int) {
  g_stop.store(true);
  // write() is async-signal-safe; the event loop reads the eventfd and wakes.
  if (int fd = g_wake_fd.load(); fd >= 0) {
    std::uint64_t one = 1;
    (void)!::write(fd, &one, sizeof one);
  }
}

enum class Phase : std::uint8_t { Body, Running, Writing };

class Engine;
struct Session;

/// An h2o timer that knows its session. h2o hands back the h2o_timer_t*, which
/// is the first member of this standard-layout struct.
struct TimerRef {
  h2o_timer_t timer;
  Session* s;
};

struct Session {
  Engine* engine = nullptr;
  h2o_req_t* req = nullptr;  // valid until dispose_session
  std::uint64_t txn = 0;     // 0 once the request no longer waits for a worker
  Phase phase = Phase::Body;
  Request meta;  // the request without its body, for early/late rejections
  std::string body;
  bool head_only = false;
  bool discarding = false;  // answered before the body was in: read and drop the rest
  std::size_t discarded = 0;
  // h2o handed over a piece of the body (req->entity) that proceed_req has not
  // released yet. h2o allows exactly one proceed_req(req, nullptr) per piece.
  bool piece_held = false;
  std::shared_ptr<std::stop_source> stop = std::make_shared<std::stop_source>();
  Response res;          // kept here until h2o has sent it
  TimerRef deadline{};   // request_timeout -> 504
  TimerRef kick{};       // the next proceed_req, run outside h2o's own callbacks
};

/// Lives in the request's memory pool; h2o calls dispose_session when it frees
/// the request, whether the response was sent or the client went away.
struct SessionRef {
  Session* s;
};

struct Handler {
  h2o_handler_t super;
  Engine* engine;
};

struct Receiver {
  h2o_multithread_receiver_t rx;
  Engine* engine;
};

/// A finished handler, posted from a worker to the event loop.
struct Done {
  h2o_multithread_message_t msg;
  std::uint64_t txn;
  Response* res;
};

h2o_generator_t g_generator = {nullptr, nullptr};  // the whole body goes in one h2o_send

const h2o_iovec_t kHttp1Alpn[] = {{const_cast<char*>("http/1.1"), 8}, {nullptr, 0}};

constexpr bool status_has_body(int st) { return !(st < 200 || st == 204 || st == 304); }

std::string_view view(h2o_iovec_t v) { return {v.base ? v.base : "", v.len}; }

class Engine {
 public:
  Engine(Crocket& app, const LaunchOptions& opts) : app_(app), opts_(opts), cfg_(app.config()) {}

  int run();

  // h2o callbacks (C function pointers into the engine).
  static int on_req_cb(h2o_handler_t* self, h2o_req_t* req) {
    return reinterpret_cast<Handler*>(self)->engine->on_req(req);
  }
  static int on_body_cb(void* ctx, int is_end_stream) {
    auto* s = static_cast<Session*>(ctx);
    return s->engine->on_body(s, is_end_stream != 0);
  }
  static void on_deadline_cb(h2o_timer_t* t) {
    auto* s = reinterpret_cast<TimerRef*>(t)->s;
    s->engine->on_deadline(s);
  }
  static void on_kick_cb(h2o_timer_t* t) {
    auto* s = reinterpret_cast<TimerRef*>(t)->s;
    s->engine->on_kick(s);
  }
  static void dispose_session(void* p) {
    auto* s = static_cast<SessionRef*>(p)->s;
    s->engine->forget(s);
  }
  static void on_completions_cb(h2o_multithread_receiver_t* rx, h2o_linklist_t* messages) {
    reinterpret_cast<Receiver*>(rx)->engine->on_completions(messages);
  }
  static void on_accept_cb(h2o_socket_t* listener, const char* err) {
    static_cast<Engine*>(listener->data)->on_accept(listener, err);
  }
  static void on_wake_cb(h2o_socket_t* sock, const char*) { h2o_buffer_consume(&sock->input, sock->input->size); }

 private:
  // --- event-loop side ----------------------------------------------------------
  int on_req(h2o_req_t* req);
  int on_body(Session* s, bool end);
  void on_deadline(Session* s);
  void on_kick(Session* s);
  void on_completions(h2o_linklist_t* messages);
  void on_accept(h2o_socket_t* listener, const char* err);
  void forget(Session* s);

  Request build_request(h2o_req_t* req, std::size_t& header_bytes) const;
  bool take_body(Session* s, h2o_iovec_t chunk);
  void dispatch(Session* s);
  void reject(Session* s, const ApiError& err) { respond(s, app_.reject(s->meta, err)); }
  void respond(Session* s, Response res);
  void kick(Session* s) {
    if (!h2o_timer_is_linked(&s->kick.timer)) h2o_timer_link(ctx_.loop, 0, &s->kick.timer);
  }

  bool setup_tls();
  int open_listener();
  [[nodiscard]] std::size_t open_connections() const {
    auto& n = ctx_._conns.num_conns;
    return n.idle + n.active + n.shutdown;
  }

  // --- worker side ---------------------------------------------------------------
  void start_workers(unsigned n);
  void worker_loop();
  bool wait_workers_idle(Clock::time_point until);
  void stop_workers();

  Crocket& app_;
  const LaunchOptions& opts_;
  const Config& cfg_;

  h2o_globalconf_t config_{};
  h2o_context_t ctx_{};
  h2o_accept_ctx_t accept_ctx_{};
  SSL_CTX* ssl_ = nullptr;
  h2o_socket_t* listener_ = nullptr;
  h2o_socket_t* wake_ = nullptr;
  Receiver done_rx_{};

  std::uint64_t next_txn_ = 1;
  std::unordered_map<std::uint64_t, Session*> live_;  // event-loop thread only

  std::mutex jobs_m_;
  std::condition_variable jobs_cv_;
  std::condition_variable idle_cv_;
  std::deque<std::move_only_function<void()>> jobs_;
  unsigned busy_ = 0;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
  std::atomic<std::size_t> outstanding_{0};  // dispatched, not yet finished by a worker
};

// ---- request construction ---------------------------------------------------------

Request Engine::build_request(h2o_req_t* req, std::size_t& header_bytes) const {
  Request rq;
  rq.protocol = req->version >= 0x200 ? std::string_view("h2") : std::string_view("http/1.1");
  rq.tls = ssl_ != nullptr;  // one listener: TLS or not, whatever :scheme claims
  rq.method_text = std::string(view(req->method));
  rq.method = http::parse_method(rq.method_text);

  // h2o has already removed dot segments and percent-decoded the path.
  auto path = view(req->path_normalized);
  rq.path = path.empty() ? std::string("/") : std::string(path);
  if (req->query_at != SIZE_MAX) rq.query = parse_query(view(req->path).substr(req->query_at + 1));

  header_bytes = req->method.len + req->path.len;
  for (std::size_t i = 0; i < req->headers.size; ++i) {
    const h2o_header_t& h = req->headers.entries[i];
    rq.headers.add(view(*h.name), view(h.value));
    header_bytes += h.name->len + h.value.len;
  }
  // h2 carries the host in :authority, which h2o keeps out of the header list.
  if (!rq.headers.contains("host") && req->input.authority.len) rq.headers.add("host", view(req->input.authority));

  sockaddr_storage ss{};
  if (socklen_t n = req->conn->callbacks->get_peername(req->conn, reinterpret_cast<sockaddr*>(&ss)); n) {
    char host[NI_MAXHOST];
    if (std::size_t len = h2o_socket_getnumerichost(reinterpret_cast<sockaddr*>(&ss), n, host); len != SIZE_MAX)
      rq.remote_addr.assign(host, len);
  }
  rq.received = Clock::now();
  return rq;
}

// ---- event loop handlers -------------------------------------------------------------

int Engine::on_req(h2o_req_t* req) {
  auto* s = new Session;
  s->engine = this;
  s->req = req;
  s->txn = next_txn_++;
  h2o_timer_init(&s->deadline.timer, on_deadline_cb);
  s->deadline.s = s;
  h2o_timer_init(&s->kick.timer, on_kick_cb);
  s->kick.s = s;
  auto* ref = static_cast<SessionRef*>(h2o_mem_alloc_shared(&req->pool, sizeof(SessionRef), dispose_session));
  ref->s = s;
  live_.emplace(s->txn, s);
  // With the body still arriving, the part in req->entity is held until proceed_req.
  s->piece_held = req->proceed_req != nullptr;

  std::size_t header_bytes = 0;
  s->meta = build_request(req, header_bytes);
  s->meta.deadline.stop = s->stop->get_token();
  app_.prepare(s->meta);  // request id + deadline, fixed before anything can fail
  s->head_only = s->meta.method == http::Method::Head;

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(s->meta.deadline.remaining()).count();
  h2o_timer_link(ctx_.loop, std::uint64_t(std::max<std::int64_t>(1, ms)), &s->deadline.timer);

  if (header_bytes > opts_.max_header_bytes) {
    reject(s, {431, "headers.too_large", "request headers too large", {}});
    return 0;
  }
  if (req->path_normalized_has_null_char) {
    reject(s, {400, "http.invalid", "invalid request path", {}});
    return 0;
  }
  if (req->content_length != SIZE_MAX && req->content_length > cfg_.max_body_bytes) {
    reject(s, {413, "body.too_large", "request body too large", {}});
    return 0;
  }
  if (!take_body(s, req->entity)) return 0;
  if (!req->proceed_req) {  // the whole body (if any) arrived with the headers
    dispatch(s);
    return 0;
  }
  // The body is still arriving: h2o hands it over in pieces through on_body.
  req->write_req.cb = on_body_cb;
  req->write_req.ctx = s;
  kick(s);
  return 0;
}

/// Appends a piece of the body, or answers 413 when it goes past the limit.
bool Engine::take_body(Session* s, h2o_iovec_t chunk) {
  if (s->body.size() + chunk.len > cfg_.max_body_bytes) {
    reject(s, {413, "body.too_large", "request body too large", {}});
    return false;
  }
  s->body.append(chunk.base ? chunk.base : "", chunk.len);
  return true;
}

int Engine::on_body(Session* s, bool end) {
  h2o_iovec_t chunk = s->req->entity;
  s->piece_held = !end;  // the last piece needs no proceed_req
  if (s->discarding) {
    s->discarded += chunk.len;
    if (!end) kick(s);
    return 0;
  }
  if (s->phase != Phase::Body || !take_body(s, chunk)) return 0;
  if (end) dispatch(s);
  else kick(s);
  return 0;
}

/// Releases the piece of the body h2o handed over and asks for the next one.
/// Runs from a zero-delay timer, so never inside one of h2o's own callbacks.
void Engine::on_kick(Session* s) {
  h2o_req_t* req = s->req;
  if (!req->proceed_req) return;
  // Answered already: h2 resets the stream (NO_ERROR) once the response is out,
  // and h1 closes the connection after it once kMaxDiscard bytes were dropped.
  // An error may be reported at any time; asking for more only for a held piece.
  bool h2 = s->meta.protocol == std::string_view("h2");
  if (s->discarding && (h2 || s->discarded > kMaxDiscard)) {
    s->piece_held = false;
    req->proceed_req(req, h2o_httpclient_error_is_eos);
  } else if (s->piece_held) {
    s->piece_held = false;
    req->proceed_req(req, nullptr);
  }
}

void Engine::dispatch(Session* s) {
  if (outstanding_.load() >= cfg_.max_in_flight) {
    reject(s, {503, "server.busy", "server is at capacity", {}});
    return;
  }
  s->phase = Phase::Running;
  Request rq = s->meta;
  rq.body = std::move(s->body);
  outstanding_.fetch_add(1);
  std::uint64_t txn = s->txn;
  {
    std::lock_guard lk(jobs_m_);
    jobs_.emplace_back([this, txn, rq = std::move(rq)]() mutable {
      auto* res = new Response;
      try {
        *res = app_.handle(std::move(rq));
      } catch (...) {  // Crocket::handle maps handler exceptions; this is a last resort
        res->status = 500;
        res->body = R"({"error":{"code":"internal","message":"internal error"}})";
        res->set_content_type("application/json");
      }
      h2o_multithread_send_message(&done_rx_.rx, &(new Done{{}, txn, res})->msg);
      outstanding_.fetch_sub(1);
    });
  }
  jobs_cv_.notify_one();
}

void Engine::on_completions(h2o_linklist_t* messages) {
  while (!h2o_linklist_is_empty(messages)) {
    auto* d = reinterpret_cast<Done*>(messages->next);
    h2o_linklist_unlink(&d->msg.link);
    std::unique_ptr<Response> res(d->res);
    auto it = live_.find(d->txn);
    delete d;
    // Not found: the client went away, or the deadline already answered 504.
    if (it != live_.end() && it->second->phase == Phase::Running) respond(it->second, std::move(*res));
  }
}

void Engine::on_deadline(Session* s) {
  if (s->phase == Phase::Writing) return;
  s->stop->request_stop();
  if (s->phase == Phase::Running) {
    // The worker keeps running until it notices the token; its late result is
    // discarded because the txn no longer maps to a waiting session.
    live_.erase(s->txn);
    s->txn = 0;
  }
  reject(s, {504, "deadline.exceeded", "request deadline exceeded", {}});
}

/// Adds `fields` to an h2o header list. The values point into Session::res,
/// which outlives the request.
void add_headers(h2o_req_t* req, h2o_headers_t* out, const Headers& fields, const Request& rq) {
  for (auto& [k, v] : fields) {
    if (k == "content-length" || k == "connection" || k == "transfer-encoding" || k == "keep-alive") continue;
    // Crocket::finish rejects these; a fairing's on_response runs after it, so check again.
    if (!http::valid_header_name(k) || !http::valid_header_value(v)) {
      std::fprintf(stderr, "crocket: request %s: dropped a response header with a character not allowed in HTTP "
                   "headers (set by an on_response fairing)\n", rq.request_id.c_str());
      continue;
    }
    h2o_add_header_by_str(&req->pool, out, k.data(), k.size(), 1, nullptr, v.data(), v.size());
  }
}

void Engine::respond(Session* s, Response res) {
  if (s->phase == Phase::Writing) return;
  if (h2o_timer_is_linked(&s->deadline.timer)) h2o_timer_unlink(&s->deadline.timer);
  s->phase = Phase::Writing;
  s->res = std::move(res);
  Response& r = s->res;
  h2o_req_t* req = s->req;

  // gRPC carries its outcome in grpc-status; HTTP status is always 200.
  auto ct = r.headers.get("content-type");
  bool grpc = ct && grpc::is_grpc_content_type(*ct);
  int status = grpc ? 200 : r.status;
  bool body_allowed = status_has_body(status);
  if (!body_allowed) r.body.clear();
  bool h2 = s->meta.protocol == std::string_view("h2");
  bool trailers = h2 && !r.trailers.empty();  // HTTP/1.1 clients get none

  req->res.status = status;
  req->res.reason = reason_phrase(status).data();  // string literals
  add_headers(req, &req->res.headers, r.headers, s->meta);
  if (trailers) add_headers(req, &req->res.trailers, r.trailers, s->meta);
  // Without a length the stream stays open for the trailers (and gRPC always
  // sends them). HEAD reports the GET body's length and sends no body.
  if (body_allowed && !grpc && !trailers) req->res.content_length = r.body.size();
  if (!h2 && app_.core().stats.draining.load()) req->http1_is_persistent = 0;

  if (req->proceed_req) {  // answered before the body was in
    s->discarding = true;
    req->write_req.cb = on_body_cb;
    req->write_req.ctx = s;
    kick(s);
  }

  h2o_start_response(req, &g_generator);
  if (s->head_only || r.body.empty()) {
    h2o_send(req, nullptr, 0, H2O_SEND_STATE_FINAL);
  } else {
    h2o_iovec_t body = h2o_iovec_init(r.body.data(), r.body.size());
    h2o_send(req, &body, 1, H2O_SEND_STATE_FINAL);
  }
}

void Engine::forget(Session* s) {
  s->stop->request_stop();  // the client is gone (or done): cancel blocking extractors
  if (h2o_timer_is_linked(&s->deadline.timer)) h2o_timer_unlink(&s->deadline.timer);
  if (h2o_timer_is_linked(&s->kick.timer)) h2o_timer_unlink(&s->kick.timer);
  if (s->txn) live_.erase(s->txn);
  delete s;
}

void Engine::on_accept(h2o_socket_t* listener, const char* err) {
  if (err) return;
  for (int i = 0; i < 64; ++i) {  // take what is queued, a bounded batch per wakeup
    h2o_socket_t* sock = h2o_evloop_socket_accept(listener);
    if (!sock) return;
    h2o_accept(&accept_ctx_, sock);
  }
}

// ---- workers ----------------------------------------------------------------------------

void Engine::start_workers(unsigned n) {
  // Workers never take SIGINT/SIGTERM; the event-loop thread does.
  sigset_t block, old;
  sigemptyset(&block);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &block, &old);
  for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { worker_loop(); });
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

void Engine::worker_loop() {
  for (;;) {
    std::move_only_function<void()> job;
    {
      std::unique_lock lk(jobs_m_);
      jobs_cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopping and drained
      job = std::move(jobs_.front());
      jobs_.pop_front();
      ++busy_;
    }
    job();
    {
      std::lock_guard lk(jobs_m_);
      --busy_;
    }
    idle_cv_.notify_all();
  }
}

bool Engine::wait_workers_idle(Clock::time_point until) {
  std::unique_lock lk(jobs_m_);
  return idle_cv_.wait_until(lk, until, [&] { return busy_ == 0 && jobs_.empty(); });
}

void Engine::stop_workers() {
  {
    std::lock_guard lk(jobs_m_);
    stopping_ = true;
  }
  jobs_cv_.notify_all();
  for (auto& w : workers_) w.join();
  workers_.clear();
}

// ---- lifecycle -------------------------------------------------------------------------

bool Engine::setup_tls() {
  ssl_ = SSL_CTX_new(TLS_server_method());
  if (!ssl_) return false;
  SSL_CTX_set_min_proto_version(ssl_, TLS1_2_VERSION);
  SSL_CTX_set_options(ssl_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  if (SSL_CTX_use_certificate_chain_file(ssl_, opts_.tls_cert.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(ssl_, opts_.tls_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(ssl_) != 1) {
    std::fprintf(stderr, "crocket: could not load TLS certificate %s and key %s\n", opts_.tls_cert.c_str(),
                 opts_.tls_key.c_str());
    return false;
  }
  h2o_ssl_register_alpn_protocols(ssl_, opts_.http2 ? h2o_alpn_protocols : kHttp1Alpn);
  return true;
}

int Engine::open_listener() {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* found = nullptr;
  auto port = std::to_string(opts_.port);
  if (int rc = getaddrinfo(opts_.host.empty() ? nullptr : opts_.host.c_str(), port.c_str(), &hints, &found); rc != 0) {
    std::fprintf(stderr, "crocket: cannot resolve %s: %s\n", opts_.host.c_str(), gai_strerror(rc));
    return -1;
  }
  int fd = -1;
  for (addrinfo* ai = found; ai && fd < 0; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 || ::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 ||
        ::listen(fd, SOMAXCONN) != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(found);
  if (fd < 0) std::fprintf(stderr, "crocket: could not listen on %s:%u\n", opts_.host.c_str(), unsigned(opts_.port));
  return fd;
}

int Engine::run() {
  bool tls = !opts_.tls_cert.empty() && !opts_.tls_key.empty();

  h2o_config_init(&config_);
  config_.server_name = h2o_iovec_init(H2O_STRLIT(""));  // no Server header
  // crocket enforces max_body_bytes itself, so that the 413 carries its error body.
  config_.max_request_entity_size = SIZE_MAX;
  // Plain-text HTTP/2 (prior knowledge or Upgrade: h2c) only when asked for.
  config_.http1.upgrade_to_http2 = opts_.h2_prior_knowledge ? 1 : 0;
  // After GOAWAY, connections still open when the drain ends are closed.
  config_.http2.graceful_shutdown_timeout = std::uint64_t(std::max<std::int64_t>(1, cfg_.drain_timeout.count()));
  h2o_hostconf_t* host = h2o_config_register_host(&config_, h2o_iovec_init(H2O_STRLIT("default")), 65535);
  h2o_pathconf_t* path = h2o_config_register_path(host, "/", 0);
  auto* handler = reinterpret_cast<Handler*>(h2o_create_handler(path, sizeof(Handler)));
  handler->super.on_req = on_req_cb;
  handler->super.supports_request_streaming = 1;  // see on_req: crocket reads the body itself
  handler->engine = this;

  auto fail = [&] {
    if (ssl_) SSL_CTX_free(ssl_);
    h2o_config_dispose(&config_);
    shut_down(app_);
    return 1;
  };
  if (tls && !setup_tls()) return fail();
  int fd = open_listener();
  if (fd < 0) return fail();
  int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    ::close(fd);
    return fail();
  }

  h2o_context_init(&ctx_, h2o_evloop_create(), &config_);
  accept_ctx_.ctx = &ctx_;
  accept_ctx_.hosts = config_.hosts;
  accept_ctx_.ssl_ctx = ssl_;
  done_rx_.engine = this;
  h2o_multithread_register_receiver(ctx_.queue, &done_rx_.rx, on_completions_cb);

  listener_ = h2o_evloop_socket_create(ctx_.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
  listener_->data = this;
  h2o_socket_read_start(listener_, on_accept_cb);
  wake_ = h2o_evloop_socket_create(ctx_.loop, wake_fd, 0);
  h2o_socket_read_start(wake_, on_wake_cb);

  unsigned n = opts_.workers ? opts_.workers : std::max(4u, std::thread::hardware_concurrency());
  start_workers(n);

  g_stop.store(false);
  g_wake_fd.store(wake_fd);
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_int{}, old_term{}, old_pipe{}, ignore{};
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);
  ignore.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &ignore, &old_pipe);  // a peer closing mid-write is an error code, not a signal

  const char* h2 = tls && opts_.http2 ? ", h2 via ALPN" : !tls && opts_.h2_prior_knowledge ? ", h2 with prior knowledge" : "";
  std::fprintf(stderr, "crocket: listening on %s://%s:%u (%u workers%s)\n", tls ? "https" : "http",
               opts_.host.c_str(), unsigned(opts_.port), n, h2);

  while (!g_stop.load()) h2o_evloop_run(ctx_.loop, INT32_MAX);

  // ---- graceful shutdown: stop accepting, drain, then drop state ----
  app_.core().stats.draining.store(true);
  h2o_socket_read_stop(listener_);
  h2o_socket_close(listener_);  // new connections are refused from here on
  listener_ = nullptr;
  h2o_context_request_shutdown(&ctx_);  // GOAWAY on h2; idle h1 connections close
  auto drain_until = Clock::now() + cfg_.drain_timeout;
  while ((!live_.empty() || outstanding_.load() > 0) && Clock::now() < drain_until) h2o_evloop_run(ctx_.loop, 50);
  bool drained = live_.empty() && outstanding_.load() == 0;

  // Anything still open is told to stop; handlers get a short grace period.
  for (auto& [txn, s] : live_) s->stop->request_stop();
  {
    std::lock_guard lk(jobs_m_);
    jobs_.clear();  // never started: their sessions go with their connections
  }
  if (!wait_workers_idle(Clock::now() + 1s)) {
    std::fprintf(stderr, "crocket: handlers still running after drain timeout; exiting without destroying state\n");
    std::fflush(stderr);
    std::quick_exit(1);
  }
  stop_workers();

  // Deliver the last completions (dropped: nobody waits), let h2o finish
  // writing, and close what is left.
  auto close_until = Clock::now() + 1s;
  do {
    h2o_context_close_idle_connections(&ctx_, SIZE_MAX, 0);
    h2o_evloop_run(ctx_.loop, 10);
  } while (open_connections() > 0 && Clock::now() < close_until);

  g_wake_fd.store(-1);
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);
  sigaction(SIGPIPE, &old_pipe, nullptr);
  h2o_socket_read_stop(wake_);
  h2o_socket_close(wake_);
  wake_ = nullptr;
  // A connection that is still open owns a request (and a session) h2o has not
  // freed; tearing the loop down under it is not safe, so it is left to exit.
  if (open_connections() == 0 && live_.empty()) {
    h2o_multithread_unregister_receiver(ctx_.queue, &done_rx_.rx);
    h2o_evloop_t* loop = ctx_.loop;
    h2o_context_dispose(&ctx_);
    h2o_evloop_destroy(loop);
    h2o_config_dispose(&config_);
  }
  if (ssl_) SSL_CTX_free(ssl_);

  shut_down(app_);
  if (!drained) std::fprintf(stderr, "crocket: drain timeout reached with requests still open\n");
  return drained ? 0 : 2;
}

}  // namespace

int run_engine(Crocket& app, const LaunchOptions& opts) {
  Engine engine(app, opts);
  return engine.run();
}

}  // namespace crocket::detail
