// Round trip of one localhost GET through http::Client. Same row format as
// bench/micro.cpp. This is the whole call (connect, headers, body), not the
// overhead over a raw h2o client.
//
//   ./dev bench client
//   ./build/crocket_bench_client

#include <crocket/client.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace crocket;
using namespace std::chrono_literals;

namespace {

int listen_port(std::uint16_t& port) {
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
  if (listen(fd, 128) != 0) return -1;
  return fd;
}

void serve(int fd, std::atomic<bool>& stop) {
  while (!stop.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv{0, 200'000};
    if (select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
    int conn = ::accept(fd, nullptr, nullptr);
    if (conn < 0) continue;
    char buf[1024];
    (void)::recv(conn, buf, sizeof buf, 0);
    static constexpr char body[] = "ok";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    (void)::send(conn, res.data(), res.size(), MSG_NOSIGNAL);
    ::close(conn);
  }
}

template <class F>
void measure(const char* name, F op, std::chrono::milliseconds budget = std::chrono::milliseconds(500)) {
  for (int i = 0; i < 20; ++i) op();
  std::uint64_t n = 0;
  auto t0 = std::chrono::steady_clock::now();
  auto until = t0 + budget;
  while (std::chrono::steady_clock::now() < until) {
    op();
    ++n;
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
  std::printf("%s\tns_per_op\t%.1f\tlower\n", name, ns);
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  std::uint16_t port = 0;
  int fd = listen_port(port);
  if (fd < 0) {
    std::fprintf(stderr, "bench listen failed\n");
    return 1;
  }
  std::atomic<bool> stop{false};
  std::thread server(serve, fd, std::ref(stop));
  http::Client client{{
      .base = "http://127.0.0.1:" + std::to_string(port),
      .name = "bench",
      .timeout = 2s,
      .pool = 8,
      .retry = retry::none,
      .breaker = breaker::none,
  }};
  auto once = sync_wait(client.get("/"));
  if (!once || once->status != 200 || once->body != "ok") {
    std::fprintf(stderr, "warmup failed\n");
    stop.store(true);
    server.join();
    ::close(fd);
    return 1;
  }
  std::fprintf(stderr, "client.get: one HTTP/1.1 GET to 127.0.0.1, connection per call (server closes).\n");
  measure("client.get", [&] {
    auto r = sync_wait(client.get("/"));
    if (!r || r->status != 200) std::fprintf(stderr, "call failed\n");
  });
  stop.store(true);
  server.join();
  ::close(fd);
  return 0;
}
