// The wire engine: libwebsockets on one event-loop thread, handlers on a
// worker pool. This is the only translation unit that includes lws.
//
// Flow per HTTP transaction (h1 request or h2 stream; each has its own wsi):
//
//   LWS_CALLBACK_HTTP             build Request from lws tokens, arm deadline timer,
//                                 reject early (413 / 411 / 503), else wait for
//                                 body or dispatch
//   LWS_CALLBACK_HTTP_BODY        append, enforce max_body_bytes
//   LWS_CALLBACK_HTTP_BODY_COMPLETION   dispatch
//   (worker)                      Crocket::handle -> completion queue -> lws_cancel_service
//   LWS_CALLBACK_EVENT_WAIT_CANCELLED   pull completions, request writeable
//   LWS_CALLBACK_HTTP_WRITEABLE   status+headers, then body in chunks, then
//                                 trailers (h2 only), then lws_http_transaction_completed
//   LWS_CALLBACK_TIMER            deadline hit: cancel token, answer 504
//   LWS_CALLBACK_CLOSED_HTTP      peer went away: cancel token, forget the txn
//
// See docs/ENGINE.md.

#include "engine.hpp"
#include "crocket/grpc.hpp"

#include <libwebsockets.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Internal lws API (declared in lws' private headers, exported from the static
// library). Marking an h2 stream "immortal" suspends the network connection's
// keepalive idle timer (vhost keepalive_timeout, default 5 s) while the stream is
// open; lws undoes it when the stream closes. Without it, a handler that runs
// longer than keepalive_timeout gets its whole h2 connection closed under it.
// It is marked once the body is in: lws re-arms a body timeout for every DATA
// frame of an upload and logs an error each time if the stream is immortal.
// Pinned to libwebsockets v4.3.5; revisit on upgrade.
extern "C" void lws_mux_mark_immortal(struct lws* wsi);

namespace crocket::detail {
namespace {

using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};
std::atomic<lws_context*> g_ctx{nullptr};

/// lws logs two ordinary h2 events at error level; drop those, keep the rest:
///  - "skint": a stream waiting for the peer's WINDOW_UPDATE (flow control);
///  - "on immortal stream": lws re-arming its body timeout on a stream the
///    engine already marked immortal, because it counted the whole body in
///    before lws finished the last frame.
extern "C" void on_lws_log(int level, const char* line) {
  if (std::strstr(line, ": skint\n") || std::strstr(line, "lws_set_timeout: on immortal stream")) return;
  lwsl_emit_stderr(level, line);
}

extern "C" void on_signal(int) {
  g_stop.store(true);
  // lws_cancel_service only writes to the context's wakeup pipe/eventfd.
  if (auto* c = g_ctx.load()) lws_cancel_service(c);
}

enum class Phase : std::uint8_t { Body, Running, Writing };

struct Session {
  std::uint64_t txn = 0;
  lws* wsi = nullptr;
  Phase phase = Phase::Body;
  Request meta;  // the request without its body, for early/late rejections
  std::string body;
  std::size_t expected_body = 0;
  bool open_ended = false;  // h2 body without a Content-Length: ends with the stream
  bool grpc_body = false;   // ...framed as one gRPC message, which says its own length
  std::shared_ptr<std::stop_source> stop = std::make_shared<std::stop_source>();

  Response res;
  bool head_only = false;
  bool headers_sent = false;
  std::size_t sent = 0;
};

struct Pss {
  Session* s;
};

struct Completed {
  std::uint64_t txn;
  Response res;
};

class Engine {
 public:
  Engine(Crocket& app, const LaunchOptions& opts) : app_(app), opts_(opts), cfg_(app.config()) {}

  int run();

  static int callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len);

 private:
  // --- event-loop side ----------------------------------------------------------
  int on_http(lws* wsi, Pss* pss, const char* uri, std::size_t len);
  int on_body(Pss* pss, const char* data, std::size_t len);
  int on_body_complete(Pss* pss);
  int on_writeable(lws* wsi, Pss* pss);
  void on_timer(Pss* pss);
  void on_closed(Pss* pss);
  void pull_completions();

  void dispatch(Session* s);
  void respond_now(Session* s, const ApiError& err);
  void forget(Pss* pss);

  Request build_request(lws* wsi, const char* uri, std::size_t len);

  // --- worker side ---------------------------------------------------------------
  void start_workers(unsigned n);
  void worker_loop();
  bool wait_workers_idle(Clock::time_point until);

  Crocket& app_;
  const LaunchOptions& opts_;
  const Config& cfg_;
  lws_context* ctx_ = nullptr;

  std::uint64_t next_txn_ = 1;
  std::unordered_map<std::uint64_t, Session*> live_;  // event-loop thread only

  std::mutex done_m_;
  std::vector<Completed> done_;

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

std::string copy_token(lws* wsi, lws_token_indexes t) {
  int n = lws_hdr_total_length(wsi, t);
  if (n <= 0) return {};
  std::string v(std::size_t(n) + 1, '\0');
  int got = lws_hdr_copy(wsi, v.data(), n + 1, t);
  v.resize(got > 0 ? std::size_t(got) : 0);
  return v;
}

struct CustomCtx {
  lws* wsi;
  Headers* headers;
};

extern "C" void custom_header_cb(const char* name, int nlen, void* opaque) {
  auto* c = static_cast<CustomCtx*>(opaque);
  int vlen = lws_hdr_custom_length(c->wsi, name, nlen);
  if (vlen < 0) return;
  std::string v(std::size_t(vlen) + 1, '\0');
  int got = lws_hdr_custom_copy(c->wsi, v.data(), vlen + 1, name, nlen);
  v.resize(got > 0 ? std::size_t(got) : 0);
  std::string_view n(name, std::size_t(nlen));
  if (!n.empty() && n.back() == ':') n.remove_suffix(1);
  c->headers->add(n, v);
}

Request Engine::build_request(lws* wsi, const char* uri, std::size_t len) {
  Request rq;
  lws* net = lws_get_network_wsi(wsi);
  bool h2 = net && net != wsi;
  rq.protocol = h2 ? std::string_view("h2") : std::string_view("http/1.1");
  rq.tls = lws_is_ssl(net ? net : wsi) != 0;

  // Method: h2 carries it in :method; h1 in the per-method URI token.
  if (h2) {
    rq.method_text = copy_token(wsi, WSI_TOKEN_HTTP_COLON_METHOD);
  } else {
    static constexpr std::pair<lws_token_indexes, const char*> kMethods[] = {
        {WSI_TOKEN_GET_URI, "GET"},       {WSI_TOKEN_POST_URI, "POST"},     {WSI_TOKEN_HEAD_URI, "HEAD"},
        {WSI_TOKEN_PUT_URI, "PUT"},       {WSI_TOKEN_PATCH_URI, "PATCH"},   {WSI_TOKEN_DELETE_URI, "DELETE"},
        {WSI_TOKEN_OPTIONS_URI, "OPTIONS"},
    };
    for (auto [tok, name] : kMethods)
      if (lws_hdr_total_length(wsi, tok) > 0) {
        rq.method_text = name;
        break;
      }
  }
  if (rq.method_text.empty()) rq.method_text = "GET";
  rq.method = http::parse_method(rq.method_text);

  // lws has already percent-decoded and normalised the path, and split and
  // decoded the query string into URI_ARGS fragments.
  std::string_view path(uri ? uri : "/", uri ? len : 1);
  if (auto q = path.find('?'); q != std::string_view::npos) path = path.substr(0, q);
  rq.path = path.empty() ? std::string("/") : std::string(path);
  for (int i = 0;; ++i) {
    int n = lws_hdr_fragment_length(wsi, WSI_TOKEN_HTTP_URI_ARGS, i);
    if (n <= 0) break;
    std::string frag(std::size_t(n) + 1, '\0');
    int got = lws_hdr_copy_fragment(wsi, frag.data(), n + 1, WSI_TOKEN_HTTP_URI_ARGS, i);
    frag.resize(got > 0 ? std::size_t(got) : 0);
    auto eq = frag.find('=');
    if (eq == std::string::npos) rq.query.emplace_back(std::move(frag), std::string{});
    else rq.query.emplace_back(frag.substr(0, eq), frag.substr(eq + 1));
  }

  // Known headers (lws tokens whose name ends in ':'; skips pseudo-headers and
  // method tokens), then unknown ones. lws keeps unknown headers for h1 only.
  for (int t = 0; t < WSI_TOKEN_COUNT; ++t) {
    auto tok = static_cast<lws_token_indexes>(t);
    const auto* name = reinterpret_cast<const char*>(lws_token_to_string(tok));
    if (!name || name[0] == ':' || !*name) continue;
    std::string_view n(name);
    if (n.back() != ':') continue;
    if (lws_hdr_total_length(wsi, tok) <= 0) continue;
    n.remove_suffix(1);
    rq.headers.add(n, copy_token(wsi, tok));
  }
  if (h2 && !rq.headers.contains("host")) {
    auto authority = copy_token(wsi, WSI_TOKEN_HTTP_COLON_AUTHORITY);
    if (!authority.empty()) rq.headers.add("host", authority);
  }
  CustomCtx cc{wsi, &rq.headers};
  lws_hdr_custom_name_foreach(wsi, custom_header_cb, &cc);

  char peer[128] = {};
  lws_get_peer_simple(net ? net : wsi, peer, sizeof peer - 1);
  rq.remote_addr = peer;
  rq.received = Clock::now();
  return rq;
}

// ---- event loop handlers -------------------------------------------------------------

/// Whether `body` holds at least the one length-prefixed message it announces.
bool grpc_message_complete(std::string_view body) {
  if (body.size() < 5) return false;
  std::size_t n = 0;
  for (int i = 1; i <= 4; ++i) n = (n << 8) | static_cast<unsigned char>(body[i]);
  return body.size() >= 5 + n;
}

/// The engine counts body bytes itself instead of trusting lws 4.3.5's
/// BODY_COMPLETION on h2, which (a) never comes when DATA arrived with the
/// HEADERS and was replayed from a buffer, as the replay decrements a different
/// counter than the one checked, and (b) comes too early without a
/// Content-Length, on the first piece of the END_STREAM frame.
bool body_complete(const Session& s) {
  if (s.grpc_body) return grpc_message_complete(s.body);
  return !s.open_ended && s.body.size() >= s.expected_body;
}

int Engine::on_http(lws* wsi, Pss* pss, const char* uri, std::size_t len) {
  if (pss->s) forget(pss);  // defensive: previous transaction never completed
  auto* s = new Session;
  s->txn = next_txn_++;
  s->wsi = wsi;
  pss->s = s;
  live_.emplace(s->txn, s);

  s->meta = build_request(wsi, uri, len);
  s->meta.deadline.stop = s->stop->get_token();
  app_.prepare(s->meta);  // request id + deadline, fixed before anything can fail
  s->head_only = s->meta.method == http::Method::Head;

  // Our deadline replaces lws' own idle timeouts for this transaction.
  lws_set_timeout(wsi, NO_PENDING_TIMEOUT, 0);
  auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(s->meta.deadline.remaining());
  lws_set_timer_usecs(wsi, std::max<lws_usec_t>(1, remaining.count()));

  auto cl = s->meta.header("content-length");
  if (!cl && s->meta.header("transfer-encoding")) {
    respond_now(s, {411, "body.length_required", "request bodies need a Content-Length", {}});
    return 0;
  }
  std::size_t want = 0;
  if (cl) {
    auto r = std::from_chars(cl->data(), cl->data() + cl->size(), want);
    if (r.ec != std::errc{} || r.ptr != cl->data() + cl->size()) {
      respond_now(s, {400, "http.invalid", "invalid Content-Length", {}});
      return 0;
    }
  }
  if (want > cfg_.max_body_bytes) {
    respond_now(s, {413, "body.too_large", "request body too large", {}});
    return 0;
  }
  // An h2 body needs no Content-Length (gRPC clients never send one): a POST
  // without it reads until END_STREAM, which lws reports as BODY_COMPLETION.
  bool open_ended = !cl && s->meta.protocol == std::string_view("h2") && s->meta.method == http::Method::Post;
  if (want == 0 && !open_ended) {
    dispatch(s);  // no body expected (a content-length: 0 completion is ignored)
    return 0;
  }
  s->expected_body = open_ended ? cfg_.max_body_bytes : want;
  s->open_ended = open_ended;
  s->grpc_body = open_ended && grpc::is_grpc_request(s->meta);
  if (!open_ended) s->body.reserve(want);
  s->phase = Phase::Body;
  return 0;
}

int Engine::on_body(Pss* pss, const char* data, std::size_t len) {
  Session* s = pss->s;
  if (!s || s->phase != Phase::Body || !data) return 0;
  if (s->body.size() + len > cfg_.max_body_bytes || s->body.size() + len > s->expected_body) {
    respond_now(s, {413, "body.too_large", "request body too large", {}});
    return 0;
  }
  s->body.append(data, len);
  if (body_complete(*s)) dispatch(s);
  return 0;
}

int Engine::on_body_complete(Pss* pss) {
  Session* s = pss->s;
  if (!s || s->phase != Phase::Body) return 0;
  // Only an open-ended body that is not gRPC needs lws to say where it ends;
  // otherwise on_body counts, and a completion before the count is premature.
  if (s->open_ended && !s->grpc_body) dispatch(s);
  else if (body_complete(*s)) dispatch(s);
  return 0;
}

void Engine::dispatch(Session* s) {
  if (outstanding_.load() >= cfg_.max_in_flight) {
    respond_now(s, {503, "server.busy", "server is at capacity", {}});
    return;
  }
  s->phase = Phase::Running;
  if (lws* net = lws_get_network_wsi(s->wsi); net && net != s->wsi) lws_mux_mark_immortal(s->wsi);
  Request rq = s->meta;
  rq.body = std::move(s->body);
  outstanding_.fetch_add(1);
  std::uint64_t txn = s->txn;
  {
    std::lock_guard lk(jobs_m_);
    jobs_.emplace_back([this, txn, rq = std::move(rq)]() mutable {
      Response res;
      try {
        res = app_.handle(std::move(rq));
      } catch (...) {  // Crocket::handle maps handler exceptions; this is a last resort
        res.status = 500;
        res.body = R"({"error":{"code":"internal","message":"internal error"}})";
        res.set_content_type("application/json");
      }
      {
        std::lock_guard lk2(done_m_);
        done_.push_back({txn, std::move(res)});
      }
      outstanding_.fetch_sub(1);
      lws_cancel_service(ctx_);
    });
  }
  jobs_cv_.notify_one();
}

void Engine::respond_now(Session* s, const ApiError& err) {
  s->res = app_.reject(s->meta, err);
  s->phase = Phase::Writing;
  lws_callback_on_writable(s->wsi);
}

void Engine::pull_completions() {
  std::vector<Completed> batch;
  {
    std::lock_guard lk(done_m_);
    batch.swap(done_);
  }
  for (auto& c : batch) {
    auto it = live_.find(c.txn);
    if (it == live_.end()) continue;  // client gone or already answered 504
    Session* s = it->second;
    if (s->phase != Phase::Running) continue;
    s->res = std::move(c.res);
    s->phase = Phase::Writing;
    lws_callback_on_writable(s->wsi);
  }
}

void Engine::on_timer(Pss* pss) {
  Session* s = pss->s;
  if (!s || s->phase == Phase::Writing) return;
  s->stop->request_stop();
  if (s->phase == Phase::Running) {
    // The worker keeps running until it notices the token; its late result is
    // discarded because the txn no longer maps to a waiting session.
    live_.erase(s->txn);
    s->txn = 0;
  }
  respond_now(s, {504, "deadline.exceeded", "request deadline exceeded", {}});
}

constexpr bool status_has_body(int st) { return !(st < 200 || st == 204 || st == 304); }


/// Writes `fields` as one HEADERS frame (h2). `status` adds :status first.
int write_header_block(lws* wsi, const Session& s, const Headers& fields, int status, bool draining, bool body_allowed,
                       bool content_length, lws_write_protocol flags) {
  const Response& r = s.res;
  std::size_t cap = 512;
  for (auto& [k, v] : fields) cap += k.size() + v.size() + 8;
  std::vector<unsigned char> buf(LWS_PRE + cap);
  unsigned char* start = buf.data() + LWS_PRE;
  unsigned char* p = start;
  unsigned char* end = buf.data() + buf.size() - 1;
  bool h1 = s.meta.protocol == std::string_view("http/1.1");

  if (status && lws_add_http_header_status(wsi, unsigned(status), &p, end)) return -1;
  for (auto& [k, v] : fields) {
    if (k == "content-length" || k == "connection" || k == "transfer-encoding" || k == "keep-alive") continue;
    // Crocket::finish rejects these; a fairing's on_response runs after it, so check again.
    if (!http::valid_header_name(k) || !http::valid_header_value(v)) {
      std::fprintf(stderr, "crocket: request %s: dropped a response header with a character not allowed in HTTP "
                   "headers (set by an on_response fairing)\n", s.meta.request_id.c_str());
      continue;
    }
    std::string name = k + ":";
    if (lws_add_http_header_by_name(wsi, reinterpret_cast<const unsigned char*>(name.c_str()),
                                    reinterpret_cast<const unsigned char*>(v.data()), int(v.size()), &p, end))
      return -1;
  }
  if (status && h1 && draining) {
    static constexpr unsigned char kName[] = "connection:";
    if (lws_add_http_header_by_name(wsi, kName, reinterpret_cast<const unsigned char*>("close"), 5, &p, end))
      return -1;
  }
  if (status && body_allowed && content_length && lws_add_http_header_content_length(wsi, r.body.size(), &p, end))
    return -1;
  if (lws_finalize_http_header(wsi, &p, end)) return -1;
  return lws_write(wsi, start, std::size_t(p - start), flags) < 0 ? -1 : 0;
}

int Engine::on_writeable(lws* wsi, Pss* pss) {
  Session* s = pss->s;
  if (!s || s->phase != Phase::Writing) return 0;
  Response& r = s->res;
  // gRPC carries its outcome in grpc-status; HTTP status is always 200.
  auto ct = r.headers.get("content-type");
  bool grpc = ct && grpc::is_grpc_content_type(*ct);
  int status = grpc ? 200 : r.status;
  bool body_allowed = status_has_body(status);
  if (!body_allowed) r.body.clear();
  bool send_body = body_allowed && !s->head_only && !r.body.empty();
  bool send_trailers = !r.trailers.empty() && s->meta.protocol == std::string_view("h2");

  if (!s->headers_sent) {
    lws_set_timer_usecs(wsi, LWS_SET_TIMER_USEC_CANCEL);
    // A Content-Length would make lws end the stream with the body, before the trailers.
    int flags = LWS_WRITE_HTTP_HEADERS | (send_body || send_trailers ? 0 : LWS_WRITE_H2_STREAM_END);
    if (write_header_block(wsi, *s, r.headers, status, app_.core().stats.draining.load(), body_allowed,
                           !grpc && !send_trailers, static_cast<lws_write_protocol>(flags)))
      return -1;
    s->headers_sent = true;
    if (send_body || send_trailers) {
      lws_callback_on_writable(wsi);
      return 0;
    }
  } else if (send_body && s->sent < r.body.size()) {
    constexpr std::size_t kChunk = 16 * 1024;
    std::size_t n = std::min(kChunk, r.body.size() - s->sent);
    // h2: never send past the peer's flow-control window (65535 bytes until it
    // grants more); doing so is a FLOW_CONTROL_ERROR that kills the connection.
    if (auto credit = lws_get_peer_write_allowance(wsi); credit >= 0) {
      if (credit == 0) {  // lws calls back once the peer's WINDOW_UPDATE arrives
        lws_callback_on_writable(wsi);
        return 0;
      }
      n = std::min(n, std::size_t(credit));
    }
    std::vector<unsigned char> buf(LWS_PRE + n);
    std::memcpy(buf.data() + LWS_PRE, r.body.data() + s->sent, n);
    bool last = s->sent + n == r.body.size();
    auto mode = last && !send_trailers ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP;
    if (lws_write(wsi, buf.data() + LWS_PRE, n, mode) < 0) return -1;
    s->sent += n;
    if (!last || send_trailers) {
      lws_callback_on_writable(wsi);
      return 0;
    }
  } else if (send_trailers) {
    auto flags = static_cast<lws_write_protocol>(LWS_WRITE_HTTP_HEADERS | LWS_WRITE_H2_STREAM_END);
    if (write_header_block(wsi, *s, r.trailers, 0, false, false, false, flags)) return -1;
  }

  forget(pss);
  return lws_http_transaction_completed(wsi) ? -1 : 0;
}

void Engine::forget(Pss* pss) {
  Session* s = pss->s;
  if (!s) return;
  if (s->txn) live_.erase(s->txn);
  delete s;
  pss->s = nullptr;
}

void Engine::on_closed(Pss* pss) {
  if (!pss || !pss->s) return;
  pss->s->stop->request_stop();  // the client is gone: cancel blocking extractors
  forget(pss);
}

int Engine::callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len) {
  auto* self = static_cast<Engine*>(lws_context_user(lws_get_context(wsi)));
  auto* pss = static_cast<Pss*>(user);
  switch (reason) {
    case LWS_CALLBACK_HTTP:
      return self->on_http(wsi, pss, static_cast<const char*>(in), len);
    case LWS_CALLBACK_HTTP_BODY:
      return self->on_body(pss, static_cast<const char*>(in), len);
    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
      return self->on_body_complete(pss);
    case LWS_CALLBACK_HTTP_WRITEABLE:
      return self->on_writeable(wsi, pss);
    case LWS_CALLBACK_TIMER:
      self->on_timer(pss);
      return 0;
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      self->pull_completions();
      return 0;
    case LWS_CALLBACK_CLOSED_HTTP:
    case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
      self->on_closed(pss);
      return 0;
    default:
      return lws_callback_http_dummy(wsi, reason, user, in, len);
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

// ---- lifecycle -------------------------------------------------------------------------

lws_protocols g_protocols[] = {
    {"crocket-http", &Engine::callback, sizeof(Pss), 0, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
};

int Engine::run() {
  lws_set_log_level(LLL_ERR, on_lws_log);

  bool tls = !opts_.tls_cert.empty() && !opts_.tls_key.empty();
  lws_context_creation_info info{};
  info.port = opts_.port;
  info.iface = (opts_.host.empty() || opts_.host == "0.0.0.0") ? nullptr : opts_.host.c_str();
  info.protocols = g_protocols;
  info.user = this;
  info.max_http_header_data = static_cast<unsigned short>(std::clamp<std::size_t>(opts_.max_header_bytes, 1024, 65535));
  info.timeout_secs = 20;
  info.vhost_name = "crocket";
  if (tls) {
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.ssl_cert_filepath = opts_.tls_cert.c_str();
    info.ssl_private_key_filepath = opts_.tls_key.c_str();
    info.alpn = opts_.http2 ? "h2,http/1.1" : "http/1.1";
  } else if (opts_.h2_prior_knowledge) {
    info.options |= LWS_SERVER_OPTION_H2_PRIOR_KNOWLEDGE;
  }

  unsigned n = opts_.workers ? opts_.workers : std::max(4u, std::thread::hardware_concurrency());
  start_workers(n);

  ctx_ = lws_create_context(&info);
  if (!ctx_) {
    std::fprintf(stderr, "crocket: could not listen on %s:%u (lws_create_context failed)\n", opts_.host.c_str(),
                 unsigned(opts_.port));
    {
      std::lock_guard lk(jobs_m_);
      stopping_ = true;
    }
    jobs_cv_.notify_all();
    for (auto& w : workers_) w.join();
    shut_down(app_);
    return 1;
  }
  g_ctx.store(ctx_);
  g_stop.store(false);
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_int{}, old_term{};
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);

  const char* h2 = tls && opts_.http2 ? ", h2 via ALPN" : !tls && opts_.h2_prior_knowledge ? ", h2 only (prior knowledge)" : "";
  std::fprintf(stderr, "crocket: listening on %s://%s:%u (%u workers%s)\n", tls ? "https" : "http",
               opts_.host.c_str(), unsigned(opts_.port), n, h2);

  while (!g_stop.load())
    if (lws_service(ctx_, 0) < 0) break;

  // ---- graceful shutdown: stop accepting, drain, then drop state ----
  app_.core().stats.draining.store(true);
  lws_context_deprecate(ctx_, nullptr);  // closes the listen socket(s)
  auto drain_until = Clock::now() + cfg_.drain_timeout;
  std::atomic<bool> ticking{true};
  std::thread ticker([&] {  // lws_service sleeps until an event; wake it to check the clock
    while (ticking.load()) {
      std::this_thread::sleep_for(100ms);
      lws_cancel_service(ctx_);
    }
  });
  while ((!live_.empty() || outstanding_.load() > 0) && Clock::now() < drain_until)
    if (lws_service(ctx_, 0) < 0) break;
  bool drained = live_.empty() && outstanding_.load() == 0;

  // Anything still open is told to stop; handlers get a short grace period.
  for (auto& [txn, s] : live_) s->stop->request_stop();
  {
    std::lock_guard lk(jobs_m_);
    stopping_ = true;
    jobs_.clear();  // never started: their sessions are dropped with the context
  }
  jobs_cv_.notify_all();
  bool idle = wait_workers_idle(Clock::now() + 1s);
  ticking.store(false);
  ticker.join();
  if (!idle) {
    std::fprintf(stderr, "crocket: %s handlers still running after drain timeout; exiting without destroying state\n",
                 "some");
    std::fflush(stderr);
    std::quick_exit(1);
  }
  for (auto& w : workers_) w.join();

  g_ctx.store(nullptr);
  lws_context_destroy(ctx_);
  ctx_ = nullptr;
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);

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
