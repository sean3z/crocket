// The outbound call runs on the server's event loop. A fresh process, so the
// client's private loop has not been started yet.

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace crocket;
using namespace std::chrono_literals;

namespace {

int listen_loopback(std::uint16_t& port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return -1;
  socklen_t len = sizeof addr;
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  port = ntohs(addr.sin_port);
  if (listen(fd, 16) != 0) return -1;
  return fd;
}

std::string once(int fd) {
  int conn = ::accept(fd, nullptr, nullptr);
  if (conn < 0) return {};
  char buf[1024];
  (void)::recv(conn, buf, sizeof buf, 0);
  std::string res = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
  (void)::send(conn, res.data(), res.size(), MSG_NOSIGNAL);
  ::close(conn);
  return "served";
}

bool wait_port(std::uint16_t port) {
  for (int i = 0; i < 100; ++i) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::close(fd);
    if (rc == 0) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

std::string http_get(std::uint16_t port, std::string_view path) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return {};
  }
  std::string req = "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  (void)::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
  std::string out;
  char buf[2048];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    out.append(buf, std::size_t(n));
  }
  ::close(fd);
  return out;
}

}  // namespace

namespace api {

[[= http::get("/out")]]
auto out(State<http::Client> upstream) -> Task<std::string> {
  auto r = co_await upstream->get("/ping");
  if (!r) co_return std::string(r.error().code);
  co_return r->body;
}

}  // namespace api

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  std::uint16_t upstream_port = 0;
  int upstream = listen_loopback(upstream_port);
  if (upstream < 0) {
    std::fprintf(stderr, "upstream listen failed\n");
    return 1;
  }
  std::thread upstream_thread([&] { once(upstream); });

  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  cfg.server.host = "127.0.0.1";
  cfg.server.port = 18771;
  Crocket app(cfg);
  app.manage(http::Client{{
         .base = "http://127.0.0.1:" + std::to_string(upstream_port),
         .name = "billing",
         .timeout = 2s,
         .pool = 4,
         .retry = retry::none,
         .breaker = breaker::none,
     }})
      .mount("/", reflect_routes<^^api>());

  std::thread server([&] { app.launch({.host = "127.0.0.1", .port = 18771, .workers = 2, .event_loops = 1}); });
  if (!wait_port(18771)) {
    std::fprintf(stderr, "server did not listen\n");
    pthread_kill(server.native_handle(), SIGTERM);
    server.join();
    upstream_thread.join();
    return 1;
  }
  auto body = http_get(18771, "/out");
  pthread_kill(server.native_handle(), SIGTERM);
  server.join();
  upstream_thread.join();
  ::close(upstream);
  if (body.find("pong") == std::string::npos) {
    std::fprintf(stderr, "loop call failed: %s\n", body.c_str());
    return 1;
  }
  std::fprintf(stderr, "server loop call ok\n");
  return 0;
}
