// SIGTERM drains and exits 0, and a body far past max_body_bytes is a 413 the
// client can read (not a TCP reset).

#include <crocket/crocket.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                                 \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    if (!(cond)) {                                                                                  \
      ++g_failures;                                                                                 \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
    }                                                                                               \
  } while (0)

namespace {

struct Slot {
  int n = 0;
};

namespace api {

[[= http::get("/slow")]]
auto slow() -> std::string {
  std::this_thread::sleep_for(400ms);
  return "ok";
}

[[= http::get("/ping")]]
auto ping(State<Pool<Slot>> pool, Deadline d) -> std::string {
  auto lease = pool->checkout(d);
  if (!lease) return "busy";
  return "ok";
}

}  // namespace api

int serve(std::uint16_t port) {
  Config cfg;
  cfg.limits.max_body_bytes = 1 << 20;
  cfg.timeouts.drain = 2s;
  cfg.timeouts.request = 5s;
  Pool<Slot> pool(2, [] { return Slot{}; });
  return build(cfg)
      .manage(std::move(pool))
      .attach(Logger{})
      .attach(Metrics{})
      .mount("/", reflect_routes<^^api>())
      .launch({.host = "127.0.0.1", .port = port, .workers = 2, .event_loops = 2});
}

std::uint16_t free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (fd >= 0) ::close(fd);
    return 0;
  }
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

bool wait_up(std::uint16_t port) {
  for (int i = 0; i < 100; ++i) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
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

struct Child {
  pid_t pid = -1;
  std::string err_path;
};

std::string exe_path() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return {};
  buf[n] = 0;
  return buf;
}

Child spawn(std::uint16_t port) {
  auto self = exe_path();
  Child c;
  c.err_path = "/tmp/crocket-resilience-" + std::to_string(port) + ".log";
  pid_t pid = ::fork();
  if (pid < 0) return c;
  if (pid == 0) {
    FILE* err = std::freopen(c.err_path.c_str(), "w", stderr);
    if (!err) _exit(127);
    std::string p = std::to_string(port);
    execl(self.c_str(), self.c_str(), "--serve", p.c_str(), nullptr);
    _exit(127);
  }
  c.pid = pid;
  return c;
}

int finish(Child& c, int signal) {
  if (signal) ::kill(c.pid, signal);
  int status = 0;
  ::waitpid(c.pid, &status, 0);
  std::string log;
  if (FILE* f = std::fopen(c.err_path.c_str(), "r")) {
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) log += buf;
    std::fclose(f);
  }
  ::unlink(c.err_path.c_str());
  if (log.find("double free") != std::string::npos || log.find("corruption") != std::string::npos) {
    std::fprintf(stderr, "  memory error on shutdown:\n%s\n", log.c_str());
    return -1;
  }
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

int connect_port(std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Write the whole request, then read. That is what Python's http.client does,
// and what used to lose the 413 once the upload passed the discard cap.
std::string post_body(std::uint16_t port, std::size_t n) {
  int fd = connect_port(port);
  if (fd < 0) return {};
  std::string head = "POST /ping HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(n) +
                     "\r\nConnection: close\r\n\r\n";
  if (::send(fd, head.data(), head.size(), MSG_NOSIGNAL) != ssize_t(head.size())) {
    ::close(fd);
    return {};
  }
  std::string chunk(64 * 1024, 'a');
  std::size_t left = n;
  while (left) {
    std::size_t m = std::min(left, chunk.size());
    ssize_t w = ::send(fd, chunk.data(), m, MSG_NOSIGNAL);
    if (w <= 0) {
      ::close(fd);
      return {};
    }
    left -= std::size_t(w);
  }
  std::string got;
  char buf[4096];
  for (;;) {
    ssize_t r = ::recv(fd, buf, sizeof buf, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    got.append(buf, std::size_t(r));
    if (got.size() > 8192) break;
  }
  ::close(fd);
  return got;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string_view(argv[1]) == "--serve") return serve(std::uint16_t(std::stoi(argv[2])));

  auto idle_port = free_port();
  CHECK(idle_port != 0);
  auto idle = spawn(idle_port);
  CHECK(idle.pid > 0);
  CHECK(wait_up(idle_port));
  // A real GET so the managed pool is checked out once before shutdown.
  {
    int fd = connect_port(idle_port);
    CHECK(fd >= 0);
    std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    CHECK(::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == ssize_t(req.size()));
    char buf[512];
    std::string got;
    for (;;) {
      ssize_t r = ::recv(fd, buf, sizeof buf, 0);
      if (r <= 0) break;
      got.append(buf, std::size_t(r));
    }
    ::close(fd);
    CHECK(got.find("HTTP/1.1 200") != std::string::npos);
  }
  CHECK(finish(idle, SIGTERM) == 0);

  auto busy_port = free_port();
  CHECK(busy_port != 0);
  auto busy = spawn(busy_port);
  CHECK(busy.pid > 0);
  CHECK(wait_up(busy_port));
  std::vector<std::thread> inflight;
  for (int i = 0; i < 8; ++i) {
    inflight.emplace_back([busy_port] {
      int fd = connect_port(busy_port);
      if (fd < 0) return;
      std::string req = "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
      ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
      char buf[1024];
      while (::recv(fd, buf, sizeof buf, 0) > 0) {}
      ::close(fd);
    });
  }
  std::this_thread::sleep_for(50ms);
  int busy_rc = finish(busy, SIGTERM);
  for (auto& t : inflight) t.join();
  CHECK(busy_rc == 0);

  auto body_port = free_port();
  CHECK(body_port != 0);
  auto body = spawn(body_port);
  CHECK(body.pid > 0);
  CHECK(wait_up(body_port));
  // Past the old 8 MiB discard cap: a write-then-read client must still see 413.
  auto big = post_body(body_port, 32u << 20);
  CHECK(big.find("HTTP/1.1 413") != std::string::npos);
  CHECK(big.find("body.too_large") != std::string::npos);
  CHECK(finish(body, SIGTERM) == 0);

  if (g_failures) {
    std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("%d checks passed\n", g_checks);
  return 0;
}
