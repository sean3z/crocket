// HTTP/2 framing against a running server: the input is a program of frames
// (HEADERS with and without content-length, DATA of any size, padding and
// END_STREAM, CONTINUATION, WINDOW_UPDATE, RST_STREAM, SETTINGS, PING,
// PRIORITY, GOAWAY and raw frames), sent over h2c to a crocket server in this
// process. Every 32 inputs the server must still answer, and every request
// it took must have finished: none left in flight by a body that was counted
// wrong.

#include "fuzz.hpp"

#include <crocket/crocket.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace crocket;
using namespace std::chrono_literals;

namespace fuzzh2 {

struct Line {
  std::string sku;
  int qty = 1;
};
struct Order {
  std::uint64_t id = 0;
  std::vector<Line> lines = {};
};
struct Hi {
  std::string name;
};
struct HiReply {
  std::string message;
};

namespace api {
[[= http::post("/echo")]]
auto echo(const Request& rq) -> std::string { return std::to_string(rq.body.size()); }
[[= http::post("/json")]]
auto json(Json<Order> o) -> Json<Order> { return o; }
[[= http::get("/nap")]]
auto nap() -> Task<std::string> {
  co_await sleep_for(5ms);
  co_return "napped";
}
[[= http::get("/slow")]]
auto slow(Deadline d) -> std::string {
  for (int i = 0; i < 4 && !d.expired(); ++i) std::this_thread::sleep_for(5ms);
  return "slept";
}
}  // namespace api

namespace greeter {
[[= grpc::rpc]]
auto say_hello(Hi h) -> HiReply { return {"hi " + h.name}; }
}  // namespace greeter

namespace {

std::uint16_t port() {
  const char* p = std::getenv("CROCKET_FUZZ_PORT");
  return p && *p ? std::uint16_t(std::atoi(p)) : 18661;
}

std::unique_ptr<Crocket> g_app;
std::thread g_server;
unsigned g_runs = 0;

int connect_server() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port());
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return fd;
}

void send_all(int fd, std::string_view s) {
  while (!s.empty()) {
    auto n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
    if (n <= 0) return;  // the server closed the connection: fine, it may
    s.remove_prefix(std::size_t(n));
  }
}

/// Reads and drops what the server sends for up to `ms`; false once it closed.
bool drain(int fd, int ms) {
  char buf[16384];
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (true) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
    if (left <= 0) return true;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, int(left)) <= 0) return true;
    auto n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
  }
}

/// GET /healthz over HTTP/1.1 on a fresh connection: the server still serves.
bool healthy() {
  for (int attempt = 0; attempt < 50; ++attempt) {
    int fd = connect_server();
    if (fd < 0) {
      std::this_thread::sleep_for(100ms);
      continue;
    }
    send_all(fd, "GET /healthz HTTP/1.1\r\nhost: localhost\r\nconnection: close\r\n\r\n");
    std::string got;
    char buf[4096];
    pollfd p{fd, POLLIN, 0};
    while (::poll(&p, 1, 3000) > 0) {
      auto n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      got.append(buf, std::size_t(n));
    }
    ::close(fd);
    return got.starts_with("HTTP/1.1 200");
  }
  return false;
}

void check_server() {
  if (!healthy()) fuzz::fail("the server stopped answering GET /healthz");
  // Requests from earlier inputs finish within their deadline (1 s) or by the
  // stream's reset; one still in flight after that was lost.
  auto until = std::chrono::steady_clock::now() + 3s;
  while (g_app->core().stats.in_flight.load() > 0 && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(10ms);
  if (auto n = g_app->core().stats.in_flight.load(); n > 0)
    fuzz::fail(std::to_string(n) + " request(s) still in flight 3 s after their connections closed");
}

void setup() {
  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  cfg.max_body_bytes = 32 << 10;
  cfg.request_timeout = 1s;
  cfg.drain_timeout = 2s;
  g_app = std::make_unique<Crocket>(cfg);
  g_app->mount("/", reflect_routes<^^api>()).mount("demo", reflect_routes<^^greeter>(), Mode::Grpc);
  g_server = std::thread([] {
    (void)g_app->launch({.host = "127.0.0.1", .port = port(), .workers = 2, .event_loops = 2, .h2_prior_knowledge = true});
  });
  if (!healthy()) fuzz::fail("the server did not start");
}

void teardown() {
  check_server();
  ::kill(::getpid(), SIGTERM);  // graceful shutdown, as in production
  g_server.join();
  g_app.reset();
}

// ---- frames ------------------------------------------------------------------------------

struct In {
  std::string_view s;
  std::size_t i = 0;
  [[nodiscard]] bool done() const { return i >= s.size(); }
  std::uint8_t u8() { return i < s.size() ? std::uint8_t(s[i++]) : 0; }
  std::uint32_t u16() { return std::uint32_t(u8()) << 8 | u8(); }
  std::uint32_t u32() { return u16() << 16 | u16(); }
  std::string_view take(std::size_t n) {
    n = std::min(n, s.size() - std::min(i, s.size()));
    auto out = s.substr(i, n);
    i += n;
    return out;
  }
};

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream, std::string_view payload) {
  std::string f;
  auto n = std::uint32_t(payload.size());
  f += char(n >> 16), f += char(n >> 8), f += char(n);
  f += char(type), f += char(flags);
  f += char((stream >> 24) & 0x7f), f += char(stream >> 16), f += char(stream >> 8), f += char(stream);
  f += payload;
  return f;
}

void hpack_int(std::string& out, std::uint32_t v, int prefix_bits, std::uint8_t first) {
  std::uint32_t max = (1u << prefix_bits) - 1;
  if (v < max) {
    out += char(first | v);
    return;
  }
  out += char(first | max);
  v -= max;
  while (v >= 128) {
    out += char((v & 0x7f) | 0x80);
    v >>= 7;
  }
  out += char(v);
}

void literal(std::string& out, std::string_view name, std::string_view value) {
  out += char(0);  // literal header field without indexing, new name
  hpack_int(out, std::uint32_t(name.size()), 7, 0);
  out += name;
  hpack_int(out, std::uint32_t(value.size()), 7, 0);
  out += value;
}

std::string header_block(In& in) {
  static constexpr std::string_view paths[] = {"/echo", "/json", "/nap", "/slow", "/demo.Greeter/SayHello", "/nope",
                                               "/healthz"};
  static constexpr std::string_view methods[] = {"POST", "GET", "PUT", "HEAD"};
  std::string b;
  auto shape = in.u8();
  literal(b, ":method", methods[shape % 4]);
  literal(b, ":scheme", "http");
  literal(b, ":path", paths[in.u8() % std::size(paths)]);
  literal(b, ":authority", "localhost");
  if (shape & 0x10) literal(b, "content-length", std::to_string(shape & 0x20 ? in.u16() : in.u8()));
  if (shape & 0x40) literal(b, "content-type", shape & 0x80 ? "application/grpc" : "application/json");
  if (shape & 0x80) literal(b, "te", "trailers");
  if ((shape & 0x0c) == 0x0c) literal(b, "x-junk", in.take(in.u8() % 64));  // raw bytes as a value
  return b;
}

void run(std::string_view input) {
  In in{input};
  int fd = connect_server();
  if (fd < 0) fuzz::fail("cannot connect to the server");
  std::string out = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  out += frame(0x4, 0, 0, {});  // SETTINGS
  std::uint32_t next = 1, last = 1;
  for (int ops = 0; ops < 48 && !in.done(); ++ops) {
    auto op = in.u8();
    auto flags = in.u8();
    std::uint32_t sid = flags & 0x40 ? 1 + 2 * (in.u8() % 8) : last;
    switch (op % 12) {
      case 0:
      case 1: {  // HEADERS, opening the next stream
        last = sid = flags & 0x80 ? sid : next;
        next = std::max(next, sid + 2);
        auto block = header_block(in);
        std::uint8_t f = (flags & 0x01) | 0x04;  // END_STREAM as chosen, END_HEADERS
        if (flags & 0x02) {                       // split: HEADERS, then CONTINUATION
          auto cut = block.size() / 2;
          out += frame(0x1, f & 0x01, sid, std::string_view(block).substr(0, cut));
          out += frame(0x9, flags & 0x04 ? 0 : 0x04, sid, std::string_view(block).substr(cut));
        } else {
          out += frame(0x1, f, sid, block);
        }
        break;
      }
      case 2:
      case 3:
      case 4: {  // DATA
        std::size_t len = flags & 0x20 ? in.u16() % 20000 : in.u8();
        std::string payload;
        std::uint8_t f = flags & 0x01;
        if (flags & 0x08) {  // PADDED
          auto pad = in.u8();
          payload += char(pad);
          payload += std::string(len, 'x');
          payload += std::string(flags & 0x10 ? pad : pad / 2, '\0');  // the padding, or less than it claims
          f |= 0x08;
        } else {
          payload = std::string(len, 'x');
        }
        out += frame(0x0, f, sid, payload);
        break;
      }
      case 5: {  // WINDOW_UPDATE
        std::uint32_t inc = flags & 0x20 ? in.u32() : in.u16();
        std::string p{char((inc >> 24) & 0x7f), char(inc >> 16), char(inc >> 8), char(inc)};
        out += frame(0x8, 0, flags & 0x10 ? 0 : sid, p);
        break;
      }
      case 6: {  // RST_STREAM
        auto code = in.u8() % 14;
        out += frame(0x3, 0, sid, std::string{0, 0, 0, char(code)});
        break;
      }
      case 7: {  // SETTINGS, or its ACK
        if (flags & 0x01) {
          out += frame(0x4, 0x01, 0, {});
          break;
        }
        std::string p;
        for (int n = in.u8() % 4; n > 0; --n) {
          auto id = std::uint16_t(1 + in.u8() % 6);
          auto v = in.u32();
          p += char(id >> 8), p += char(id);
          p += char(v >> 24), p += char(v >> 16), p += char(v >> 8), p += char(v);
        }
        out += frame(0x4, 0, 0, p);
        break;
      }
      case 8:  // PING
        out += frame(0x6, flags & 0x01, 0, std::string(8, '\x01'));
        break;
      case 9:  // PRIORITY, possibly on itself
        out += frame(0x2, 0, sid, std::string{0, 0, 0, char(flags & 0x10 ? sid : 0), char(in.u8())});
        break;
      case 10: {  // a raw frame
        auto type = in.u8();
        out += frame(type, in.u8(), sid, in.take(in.u8()));
        break;
      }
      default:  // send what is queued, and let the server answer
        send_all(fd, out);
        out.clear();
        if (!drain(fd, 2)) {
          ::close(fd);
          return;
        }
    }
  }
  send_all(fd, out);
  ::shutdown(fd, SHUT_WR);  // done sending: the server answers what it has, then closes
  drain(fd, 50);
  ::close(fd);
  if (++g_runs % 32 == 0) check_server();
}

}  // namespace

fuzz::Register h2_target{{
    .name = "h2",
    .max_len = 1024,
    .run = run,
    .setup = setup,
    .teardown = teardown,
    .timeout_s = 10,
}};

}  // namespace fuzzh2
