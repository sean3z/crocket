// The wire engine: h2o (libh2o-evloop) on several event-loop threads, each with
// its own listening socket on the port, and handlers on a shared worker pool.
// This is the only translation unit that includes h2o.
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
#include "cpus.hpp"
#include "crocket/grpc.hpp"

#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>
#include <h2o/httpclient.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <functional>
#include <latch>
#include <optional>
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
constexpr std::size_t kMaxLoops = 256;
std::array<std::atomic<int>, kMaxLoops> g_wake_fds{};  // each loop's eventfd, -1 when unused
std::atomic<std::size_t> g_wake_count{0};

extern "C" void on_signal(int) {
  g_stop.store(true);
  // write() is async-signal-safe; each event loop reads its eventfd and wakes.
  for (std::size_t i = 0, n = g_wake_count.load(); i < n; ++i)
    if (int fd = g_wake_fds[i].load(); fd >= 0) {
      std::uint64_t one = 1;
      (void)!::write(fd, &one, sizeof one);
    }
}

enum class Phase : std::uint8_t { Body, Running, Writing };

class Loop;
struct Session;

/// An h2o timer that knows its session. h2o hands back the h2o_timer_t*, which
/// is the first member of this standard-layout struct.
struct TimerRef {
  h2o_timer_t timer;
  Session* s;
};

struct Session {
  Loop* loop = nullptr;
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

struct Receiver {
  h2o_multithread_receiver_t rx;
  Loop* loop;
};

/// A finished handler, posted from a worker to the event loop.
struct Done {
  h2o_multithread_message_t msg;
  std::uint64_t txn;
  Response* res;
};

/// A job for the event loop itself: an async handler to resume there.
struct LoopJob {
  h2o_multithread_message_t msg;
  std::move_only_function<void()> run;
};

h2o_generator_t g_generator = {nullptr, nullptr};  // the whole body goes in one h2o_send

const h2o_iovec_t kHttp1Alpn[] = {{const_cast<char*>("http/1.1"), 8}, {nullptr, 0}};

constexpr bool status_has_body(int st) { return !(st < 200 || st == 204 || st == 304); }

std::string_view view(h2o_iovec_t v) { return {v.base ? v.base : "", v.len}; }

class Loop;

/// What every event loop shares: h2o's configuration, TLS, the worker pool and
/// its queues, and the counts the drain waits on.
class Engine final : public Executor {
 public:
  Engine(Crocket& app, const LaunchOptions& opts) : app_(app), opts_(opts), cfg_(app.config()) {}

  int run();

  /// Executor: resumes a suspended handler on a worker. Ahead of new requests,
  /// and kept at shutdown (unlike requests that have not started).
  void post(std::move_only_function<void()> job) override {
    {
      std::lock_guard lk(jobs_m_);
      resumes_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
  }

  static int on_req_cb(h2o_handler_t* self, h2o_req_t* req);

 private:
  friend class Loop;

  bool setup_tls();
  int open_listener(std::uint16_t port, bool reuse_port);

  // --- worker side ---------------------------------------------------------------
  void start_workers(unsigned n);
  void worker_loop();
  bool wait_workers_idle(Clock::time_point until);
  void stop_workers();
  void queue_job(std::move_only_function<void()> job) {
    {
      std::lock_guard lk(jobs_m_);
      jobs_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
  }

  Crocket& app_;
  const LaunchOptions& opts_;
  const Config& cfg_;

  h2o_globalconf_t config_{};
  SSL_CTX* ssl_ = nullptr;
  std::vector<std::unique_ptr<Loop>> loops_;

  std::mutex jobs_m_;
  std::condition_variable jobs_cv_;
  std::condition_variable idle_cv_;
  std::deque<std::move_only_function<void()>> jobs_;     // new requests
  std::deque<std::move_only_function<void()>> resumes_;  // suspended handlers to continue
  unsigned busy_ = 0;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
  std::atomic<std::size_t> outstanding_{0};  // dispatched, response not yet delivered

  // Shutdown, in phases the loops and run() take in turn.
  std::optional<std::latch> loops_drained_;
  std::atomic<bool> workers_stopped_{false};
};

/// One event loop: an h2o context, its own listening socket (SO_REUSEPORT
/// spreads connections across loops), and every request on its connections.
/// Everything here runs on the loop's thread, except the completion messages
/// workers post to it.
class Loop final : public Executor {
 public:
  Loop(Engine& e, int listen_fd);
  ~Loop() override;

  /// Executor: runs `job` on this loop's thread (async handlers resume here).
  void post(std::move_only_function<void()> job) override {
    h2o_multithread_send_message(&jobs_rx_.rx, &(new LoopJob{{}, std::move(job)})->msg);
  }

  /// The loop's thread: serve until stop, then drain and close (see Engine::run).
  void serve();
  [[nodiscard]] bool drained() const { return drained_; }
  [[nodiscard]] bool cleaned_up() const { return cleaned_up_; }
  [[nodiscard]] int wake_fd() const { return wake_fd_; }

  // h2o callbacks (C function pointers into the loop).
  int on_req(h2o_req_t* req);
  static int on_body_cb(void* ctx, int is_end_stream) {
    auto* s = static_cast<Session*>(ctx);
    return s->loop->on_body(s, is_end_stream != 0);
  }
  static void on_deadline_cb(h2o_timer_t* t) {
    auto* s = reinterpret_cast<TimerRef*>(t)->s;
    s->loop->on_deadline(s);
  }
  static void on_kick_cb(h2o_timer_t* t) {
    auto* s = reinterpret_cast<TimerRef*>(t)->s;
    s->loop->on_kick(s);
  }
  static void dispose_session(void* p) {
    auto* s = static_cast<SessionRef*>(p)->s;
    s->loop->forget(s);
  }
  static void on_completions_cb(h2o_multithread_receiver_t* rx, h2o_linklist_t* messages) {
    reinterpret_cast<Receiver*>(rx)->loop->on_completions(messages);
  }
  static void on_jobs_cb(h2o_multithread_receiver_t*, h2o_linklist_t* messages) {
    while (!h2o_linklist_is_empty(messages)) {
      auto* j = reinterpret_cast<LoopJob*>(messages->next);
      h2o_linklist_unlink(&j->msg.link);
      std::unique_ptr<LoopJob> owned(j);
      owned->run();
    }
  }
  static void on_accept_cb(h2o_socket_t* listener, const char* err) {
    static_cast<Loop*>(listener->data)->on_accept(listener, err);
  }
  static void on_wake_cb(h2o_socket_t* sock, const char*) { h2o_buffer_consume(&sock->input, sock->input->size); }

 private:
  int on_body(Session* s, bool end);
  void on_deadline(Session* s);
  void on_kick(Session* s);
  void on_completions(h2o_linklist_t* messages);
  void complete(std::uint64_t txn, Response&& res);
  void on_accept(h2o_socket_t* listener, const char* err);
  void forget(Session* s);

  Request build_request(h2o_req_t* req, std::size_t& header_bytes, std::size_t& header_count) const;
  bool take_body(Session* s, h2o_iovec_t chunk);
  void dispatch(Session* s);
  void reject(Session* s, const ApiError& err) { respond(s, app_.reject(s->meta, err)); }
  void respond(Session* s, Response res);
  void kick(Session* s) {
    if (!h2o_timer_is_linked(&s->kick.timer)) h2o_timer_link(ctx_.loop, 0, &s->kick.timer);
  }
  [[nodiscard]] std::size_t open_connections() const {
    auto& n = ctx_._conns.num_conns;
    return n.idle + n.active + n.shutdown;
  }

  Engine& e_;
  Crocket& app_;
  const LaunchOptions& opts_;
  const Config& cfg_;

  h2o_context_t ctx_{};
  h2o_accept_ctx_t accept_ctx_{};
  h2o_socket_t* listener_ = nullptr;
  h2o_socket_t* wake_ = nullptr;
  int wake_fd_ = -1;
  Receiver done_rx_{};
  Receiver jobs_rx_{};
  bool drained_ = false;
  bool cleaned_up_ = false;

  std::uint64_t next_txn_ = 1;
  std::unordered_map<std::uint64_t, Session*> live_;
};

/// The loop that runs on this thread (h2o calls one handler for every context).
thread_local Loop* t_loop = nullptr;

int Engine::on_req_cb(h2o_handler_t*, h2o_req_t* req) { return t_loop->on_req(req); }

// ---- request construction ---------------------------------------------------------

Request Loop::build_request(h2o_req_t* req, std::size_t& header_bytes, std::size_t& header_count) const {
  Request rq;
  rq.protocol = req->version >= 0x200 ? std::string_view("h2") : std::string_view("http/1.1");
  rq.tls = e_.ssl_ != nullptr;  // TLS or not for every listener, whatever :scheme claims
  rq.method_text = std::string(view(req->method));
  rq.method = http::parse_method(rq.method_text);

  // h2o has already removed dot segments and percent-decoded the path.
  auto path = view(req->path_normalized);
  rq.path = path.empty() ? std::string("/") : std::string(path);
  if (req->query_at != SIZE_MAX) rq.query = parse_query(view(req->path).substr(req->query_at + 1));

  header_bytes = req->method.len + req->path.len;
  header_count = req->headers.size;
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
      rq.peer_addr.assign(host, len);
  }
  rq.received = Clock::now();
  return rq;
}

// ---- event loop handlers -------------------------------------------------------------

int Loop::on_req(h2o_req_t* req) {
  auto* s = new Session;
  s->loop = this;
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

  std::size_t header_bytes = 0, header_count = 0;
  s->meta = build_request(req, header_bytes, header_count);
  s->meta.deadline.stop = s->stop->get_token();
  app_.prepare(s->meta);  // request id + deadline, fixed before anything can fail
  s->head_only = s->meta.method == http::Method::Head;

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(s->meta.deadline.remaining()).count();
  h2o_timer_link(ctx_.loop, std::uint64_t(std::max<std::int64_t>(1, ms)), &s->deadline.timer);

  if (header_bytes > opts_.max_header_bytes || header_count > opts_.max_header_count) {
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
bool Loop::take_body(Session* s, h2o_iovec_t chunk) {
  if (s->body.size() + chunk.len > cfg_.max_body_bytes) {
    reject(s, {413, "body.too_large", "request body too large", {}});
    return false;
  }
  s->body.append(chunk.base ? chunk.base : "", chunk.len);
  return true;
}

int Loop::on_body(Session* s, bool end) {
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
void Loop::on_kick(Session* s) {
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

void Loop::dispatch(Session* s) {
  if (e_.outstanding_.load() >= cfg_.max_in_flight) {
    reject(s, {503, "server.busy", "server is at capacity", {}});
    return;
  }
  s->phase = Phase::Running;
  Request rq = s->meta;
  rq.body = std::move(s->body);
  e_.outstanding_.fetch_add(1);
  std::uint64_t txn = s->txn;
  if (app_.loop_route(rq.method, rq.path)) {
    // An async handler (or a plain function proved fast) runs here, on the loop, with no handoff. The response
    // comes back here: now, or when the task finishes (as a loop job).
    app_.handle_async(std::move(rq), [this, txn](Response&& res) {
      e_.outstanding_.fetch_sub(1);
      if (t_loop == this) complete(txn, std::move(res));
      else h2o_multithread_send_message(&done_rx_.rx, &(new Done{{}, txn, new Response(std::move(res))})->msg);
    }, this);
    return;
  }
  e_.queue_job([this, txn, rq = std::move(rq)]() mutable {
    // The response comes back to this loop, from this worker or, when an async
    // handler finishes, from another.
    auto deliver = [this, txn](Response&& res) {
      h2o_multithread_send_message(&done_rx_.rx, &(new Done{{}, txn, new Response(std::move(res))})->msg);
      e_.outstanding_.fetch_sub(1);
    };
    try {
      app_.handle_async(std::move(rq), deliver);
    } catch (...) {  // handle_async maps handler exceptions; this is a last resort
      Response res;
      res.status = 500;
      res.body = R"({"title":"Internal Server Error","status":500,"detail":"internal error","code":"internal"})";
      res.set_content_type("application/problem+json");
      deliver(std::move(res));
    }
  });
}

void Loop::on_completions(h2o_linklist_t* messages) {
  while (!h2o_linklist_is_empty(messages)) {
    auto* d = reinterpret_cast<Done*>(messages->next);
    h2o_linklist_unlink(&d->msg.link);
    std::unique_ptr<Response> res(d->res);
    std::uint64_t txn = d->txn;
    delete d;
    complete(txn, std::move(*res));
  }
}

void Loop::complete(std::uint64_t txn, Response&& res) {
  auto it = live_.find(txn);
  // Not found: the client went away, or the deadline already answered 504.
  if (it != live_.end() && it->second->phase == Phase::Running) respond(it->second, std::move(res));
}

void Loop::on_deadline(Session* s) {
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

void Loop::respond(Session* s, Response res) {
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

void Loop::forget(Session* s) {
  s->stop->request_stop();  // the client is gone (or done): cancel blocking extractors
  if (h2o_timer_is_linked(&s->deadline.timer)) h2o_timer_unlink(&s->deadline.timer);
  if (h2o_timer_is_linked(&s->kick.timer)) h2o_timer_unlink(&s->kick.timer);
  if (s->txn) live_.erase(s->txn);
  delete s;
}

void Loop::on_accept(h2o_socket_t* listener, const char* err) {
  if (err) return;
  for (int i = 0; i < 64; ++i) {  // take what is queued, a bounded batch per wakeup
    h2o_socket_t* sock = h2o_evloop_socket_accept(listener);
    if (!sock) return;
    h2o_accept(&accept_ctx_, sock);
  }
}

// ---- workers ----------------------------------------------------------------------------

void Engine::start_workers(unsigned n) {
  // Workers never take SIGINT/SIGTERM; Engine::run's thread does.
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
      jobs_cv_.wait(lk, [&] { return stopping_ || !jobs_.empty() || !resumes_.empty(); });
      auto& q = !resumes_.empty() ? resumes_ : jobs_;  // finish started requests first
      if (q.empty()) return;  // stopping and drained
      job = std::move(q.front());
      q.pop_front();
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
  // Suspended handlers count: they are not on a worker, but their request is
  // open. They may finish on an event loop, which does not notify idle_cv_, so
  // look again every few milliseconds.
  auto idle = [&] { return busy_ == 0 && jobs_.empty() && resumes_.empty() && app_.core().stats.in_flight.load() == 0; };
  while (!idle()) {
    if (Clock::now() >= until) return false;
    idle_cv_.wait_for(lk, 5ms);
  }
  return true;
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

int Engine::open_listener(std::uint16_t listen_port, bool reuse_port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* found = nullptr;
  auto port = std::to_string(listen_port);
  if (int rc = getaddrinfo(opts_.host.empty() ? nullptr : opts_.host.c_str(), port.c_str(), &hints, &found); rc != 0) {
    std::fprintf(stderr, "crocket: cannot resolve %s: %s\n", opts_.host.c_str(), gai_strerror(rc));
    return -1;
  }
  int fd = -1;
  for (addrinfo* ai = found; ai && fd < 0; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        (reuse_port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0) ||
        ::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd, SOMAXCONN) != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(found);
  if (fd < 0) std::fprintf(stderr, "crocket: could not listen on %s:%u\n", opts_.host.c_str(), unsigned(opts_.port));
  return fd;
}

// ---- one event loop ------------------------------------------------------------------

Loop::Loop(Engine& e, int listen_fd) : e_(e), app_(e.app_), opts_(e.opts_), cfg_(e.cfg_) {
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  h2o_context_init(&ctx_, h2o_evloop_create(), &e_.config_);
  accept_ctx_.ctx = &ctx_;
  accept_ctx_.hosts = e_.config_.hosts;
  accept_ctx_.ssl_ctx = e_.ssl_;
  done_rx_.loop = this;
  h2o_multithread_register_receiver(ctx_.queue, &done_rx_.rx, on_completions_cb);
  jobs_rx_.loop = this;
  h2o_multithread_register_receiver(ctx_.queue, &jobs_rx_.rx, on_jobs_cb);
  listener_ = h2o_evloop_socket_create(ctx_.loop, listen_fd, H2O_SOCKET_FLAG_DONT_READ);
  listener_->data = this;
  h2o_socket_read_start(listener_, on_accept_cb);
  wake_ = h2o_evloop_socket_create(ctx_.loop, wake_fd_, 0);
  h2o_socket_read_start(wake_, on_wake_cb);
}

Loop::~Loop() {
  // A connection that is still open owns a request (and a session) h2o has not
  // freed; tearing the loop down under it is not safe, so it is left to exit.
  if (!cleaned_up_) return;
  h2o_multithread_unregister_receiver(ctx_.queue, &done_rx_.rx);
  h2o_multithread_unregister_receiver(ctx_.queue, &jobs_rx_.rx);
  h2o_evloop_t* loop = ctx_.loop;
  h2o_context_dispose(&ctx_);
  h2o_evloop_destroy(loop);
}

void Loop::serve() {
  t_loop = this;
  while (!g_stop.load()) h2o_evloop_run(ctx_.loop, INT32_MAX);

  // 1. Stop accepting and drain this loop's requests (Engine::run waits for every loop).
  h2o_socket_read_stop(listener_);
  h2o_socket_close(listener_);  // new connections are refused from here on
  listener_ = nullptr;
  h2o_context_request_shutdown(&ctx_);  // GOAWAY on h2; idle h1 connections close
  auto drain_until = Clock::now() + cfg_.drain_timeout;
  while ((!live_.empty() || e_.outstanding_.load() > 0) && Clock::now() < drain_until) h2o_evloop_run(ctx_.loop, 50);
  drained_ = live_.empty() && e_.outstanding_.load() == 0;
  for (auto& [txn, s] : live_) s->stop->request_stop();  // what is left is told to stop
  e_.loops_drained_->count_down();

  // 2. Engine::run waits for the handlers and stops the workers; keep delivering meanwhile.
  while (!e_.workers_stopped_.load()) h2o_evloop_run(ctx_.loop, 10);

  // 3. Deliver the last completions (dropped: nobody waits), let h2o finish
  // writing, and close what is left.
  auto close_until = Clock::now() + 1s;
  do {
    h2o_context_close_idle_connections(&ctx_, SIZE_MAX, 0);
    h2o_evloop_run(ctx_.loop, 10);
  } while (open_connections() > 0 && Clock::now() < close_until);
  h2o_socket_read_stop(wake_);
  h2o_socket_close(wake_);
  wake_ = nullptr;
  cleaned_up_ = open_connections() == 0 && live_.empty();
  t_loop = nullptr;
}

// ---- the engine ------------------------------------------------------------------------

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
  auto* handler = h2o_create_handler(path, sizeof(h2o_handler_t));
  handler->on_req = on_req_cb;
  handler->supports_request_streaming = 1;  // see on_req: crocket reads the body itself

  auto fail = [&](std::vector<int> fds) {
    for (int fd : fds) ::close(fd);
    if (ssl_) SSL_CTX_free(ssl_);
    h2o_config_dispose(&config_);
    shut_down(app_);
    return 1;
  };
  if (tls && !setup_tls()) return fail({});

  // One listening socket per event loop, all on the same port (SO_REUSEPORT):
  // the kernel spreads new connections across them.
  unsigned cores = std::max(1u, std::thread::hardware_concurrency());
  unsigned n_loops = std::min<unsigned>(opts_.event_loops ? opts_.event_loops : default_event_loops(), kMaxLoops);
  std::vector<int> fds;
  std::uint16_t port = opts_.port;
  for (unsigned i = 0; i < n_loops; ++i) {
    int fd = open_listener(port, n_loops > 1);
    if (fd < 0) return fail(fds);
    fds.push_back(fd);
    if (port == 0) {  // an ephemeral port: the other loops take the same one
      sockaddr_storage ss{};
      socklen_t len = sizeof ss;
      getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len);
      port = ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                            : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    }
  }

  unsigned n_workers = opts_.workers ? opts_.workers : std::max(4u, cores);
  start_workers(n_workers);
  app_.core().executor.store(this);

  for (int fd : fds) loops_.push_back(std::make_unique<Loop>(*this, fd));
  loops_drained_.emplace(std::ptrdiff_t(n_loops));
  workers_stopped_.store(false);
  g_stop.store(false);
  for (std::size_t i = 0; i < loops_.size(); ++i) g_wake_fds[i].store(loops_[i]->wake_fd());
  g_wake_count.store(loops_.size());

  // SIGINT and SIGTERM stay blocked everywhere but in sigsuspend below: the
  // event loops and workers never take them.
  sigset_t block, old_mask;
  sigemptyset(&block);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &block, &old_mask);
  std::vector<std::thread> threads;
  for (auto& loop : loops_) threads.emplace_back([l = loop.get()] { l->serve(); });

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_int{}, old_term{}, old_pipe{}, ignore{};
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);
  ignore.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &ignore, &old_pipe);  // a peer closing mid-write is an error code, not a signal

  const char* h2 = tls && opts_.http2 ? ", h2 via ALPN" : !tls && opts_.h2_prior_knowledge ? ", h2 with prior knowledge" : "";
  std::fprintf(stderr, "crocket: listening on %s://%s:%u (%u event loops, %u workers%s)\n", tls ? "https" : "http",
               opts_.host.c_str(), unsigned(opts_.port), n_loops, n_workers, h2);

  // ---- graceful shutdown: every loop stops accepting and drains, then the workers stop ----
  // Wait for a signal. sigsuspend unblocks them only while it waits, so one
  // arriving between the check and the wait is not lost.
  sigset_t waiting = old_mask;
  sigdelset(&waiting, SIGINT);
  sigdelset(&waiting, SIGTERM);
  while (!g_stop.load()) sigsuspend(&waiting);  // the handler sets g_stop and wakes the loops
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  app_.core().stats.draining.store(true);
  loops_drained_->wait();
  bool drained = std::ranges::all_of(loops_, [](auto& l) { return l->drained(); });
  {
    std::lock_guard lk(jobs_m_);
    jobs_.clear();  // never started: their sessions go with their connections
  }
  if (!wait_workers_idle(Clock::now() + 1s)) {
    std::fprintf(stderr, "crocket: handlers still running after drain timeout; exiting without destroying state\n");
    std::fflush(stderr);
    std::quick_exit(1);
  }
  app_.core().executor.store(nullptr);
  stop_workers();
  workers_stopped_.store(true);
  for (auto& t : threads) t.join();

  g_wake_count.store(0);
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);
  sigaction(SIGPIPE, &old_pipe, nullptr);
  bool all_clean = std::ranges::all_of(loops_, [](auto& l) { return l->cleaned_up(); });
  loops_.clear();  // a loop with connections still open is left as it is (see ~Loop)
  if (all_clean) h2o_config_dispose(&config_);
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
