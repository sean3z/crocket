// h2o on its own, for ./dev bench --vs-h2o: the floor under crocket_bench. The
// same engine setup as crocket's (one event loop per physical core, each with
// its own SO_REUSEPORT listener; h2c with prior knowledge), and a handler that
// answers from constants: no routing, no JSON, no headers but content-type.
// The gap to crocket_bench is what crocket adds to a request.
//
//   GET  /plaintext, /async/plaintext                    Hello, World!
//   GET  /json, /async/json                              {"message":"Hello, World!"}
//   POST /orders, /async/orders                          the request body, echoed
//   GET  /api/v39/users/7/orders/9, /async/api/v39/...   16
//
//   CROCKET_PORT (18600) and CROCKET_EVENT_LOOPS (0: one per physical core), as for crocket_bench.

#include <h2o.h>

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#include "cpus.hpp"

namespace {

h2o_globalconf_t g_config;

void reply(h2o_req_t* req, std::string_view type, std::string_view body) {
  req->res.status = 200;
  req->res.reason = "OK";
  req->res.content_length = body.size();  // as crocket sends it: not chunked
  h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, nullptr, type.data(), type.size());
  h2o_send_inline(req, body.data(), body.size());
}

int on_req(h2o_handler_t*, h2o_req_t* req) {
  std::string_view path(req->path_normalized.base, req->path_normalized.len);
  std::string_view method(req->method.base, req->method.len);
  if (path.starts_with("/async/")) path.remove_prefix(6);
  if (method == "GET" && path == "/plaintext") reply(req, "text/plain; charset=utf-8", "Hello, World!");
  else if (method == "GET" && path == "/json") reply(req, "application/json", R"({"message":"Hello, World!"})");
  else if (method == "POST" && path == "/orders") reply(req, "application/json", {req->entity.base, req->entity.len});
  else if (method == "GET" && path == "/api/v39/users/7/orders/9") reply(req, "text/plain; charset=utf-8", "16");
  else return -1;  // h2o's 404
  return 0;
}

int listen_on(unsigned port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int on = 1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0 ||
      ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, SOMAXCONN) != 0) {
    std::perror("h2o-bench: listen");
    std::exit(1);
  }
  return fd;
}

void serve(int listen_fd) {
  h2o_context_t ctx;
  h2o_context_init(&ctx, h2o_evloop_create(), &g_config);
  h2o_accept_ctx_t accept_ctx{};
  accept_ctx.ctx = &ctx;
  accept_ctx.hosts = g_config.hosts;
  h2o_socket_t* listener = h2o_evloop_socket_create(ctx.loop, listen_fd, H2O_SOCKET_FLAG_DONT_READ);
  listener->data = &accept_ctx;
  h2o_socket_read_start(listener, [](h2o_socket_t* l, const char* err) {
    if (err) return;
    for (int i = 0; i < 64; ++i) {  // as crocket's loops do
      h2o_socket_t* sock = h2o_evloop_socket_accept(l);
      if (!sock) return;
      h2o_accept(static_cast<h2o_accept_ctx_t*>(l->data), sock);
    }
  });
  while (true) h2o_evloop_run(ctx.loop, INT32_MAX);
}

unsigned env_uint(const char* name, unsigned fallback) {
  const char* v = std::getenv(name);
  return v && *v ? unsigned(std::strtoul(v, nullptr, 10)) : fallback;
}

}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  h2o_config_init(&g_config);
  g_config.server_name = h2o_iovec_init(H2O_STRLIT(""));
  g_config.http1.upgrade_to_http2 = 1;  // h2c with prior knowledge, as crocket_bench
  h2o_hostconf_t* host = h2o_config_register_host(&g_config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
  h2o_pathconf_t* path = h2o_config_register_path(host, "/", 0);
  h2o_create_handler(path, sizeof(h2o_handler_t))->on_req = on_req;

  unsigned port = env_uint("CROCKET_PORT", 18600);
  unsigned n = env_uint("CROCKET_EVENT_LOOPS", 0);
  if (n == 0) n = crocket::detail::default_event_loops();
  std::vector<std::thread> loops;
  for (unsigned i = 0; i < n; ++i) loops.emplace_back(serve, listen_on(port));
  std::fprintf(stderr, "h2o-bench: listening on http://127.0.0.1:%u (%u event loops)\n", port, n);
  for (auto& t : loops) t.join();
}
