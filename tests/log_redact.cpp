// Diagnostic redaction. Canaries are fake. The bytes a client sends are checked
// against a local listener so a log view cannot be what went on the wire.

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

static bool contains(std::string_view h, std::string_view n) { return h.find(n) != h.npos; }

static std::mutex g_mu;
static std::string g_lines;
static std::atomic<int> g_sinks{0};

static void take(std::string_view line) {
  g_sinks.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lk(g_mu);
  g_lines.append(line);
  g_lines.push_back('\n');
}

static std::string lines() {
  std::lock_guard lk(g_mu);
  return g_lines;
}

static void clear_lines() {
  std::lock_guard lk(g_mu);
  g_lines.clear();
  g_sinks.store(0);
}

static Config quiet() {
  Config cfg;
  cfg.log.sink = take;
  cfg.log.level = log::Level::debug;
  return cfg;
}

struct Bag {
  std::string authorization;
  std::string note;
};

int main() {
  std::fprintf(stderr, "[field names]\n");
  {
    Crocket app(quiet());
    if (auto ig = app.ignite(); !ig) return 1;
    clear_lines();
    log::warn("named {authorization}", std::string("Bearer CANARYAUTH"));
    log::warn("hyphen {x_api_key}", std::string("CANARYKEY"));
    log::warn("raw {value}", std::string("Bearer CANARYAUTH"));
    std::map<std::string, std::string> hdrs{{"Authorization", "CANARYAUTH"},
                                            {"X-Api-Key", "CANARYKEY"},
                                            {"x-api-key", "CANARYKEY2"},
                                            {"Cookie", "CANARYCOOKIE"},
                                            {"Set-Cookie", "CANARYSET"},
                                            {"Proxy-Authorization", "CANARYPROXY"},
                                            {"X-Auth-Token", "CANARYTOKEN"},
                                            {"X-Request-Id", "req-1"}};
    log::warn("map {hdrs}", hdrs);
    log::warn("bag {bag}", Bag{"CANARYSTRUCT", "CANARYNOTE"});
    app.flush_logs();
    auto all = lines();
    CHECK(contains(all, R"("authorization":"[redacted]")"));
    CHECK(contains(all, R"("x_api_key":"[redacted]")"));
    CHECK(contains(all, R"("X-Api-Key":"[redacted]")"));
    CHECK(contains(all, R"("x-api-key":"[redacted]")"));
    CHECK(contains(all, R"("Cookie":"[redacted]")"));
    CHECK(contains(all, R"("Set-Cookie":"[redacted]")"));
    CHECK(contains(all, R"("Proxy-Authorization":"[redacted]")"));
    CHECK(contains(all, R"("X-Auth-Token":"[redacted]")"));
    CHECK(contains(all, R"("X-Request-Id":"req-1")"));
    CHECK(contains(all, R"("note":"CANARYNOTE")"));
    CHECK(contains(all, "Bearer CANARYAUTH"));  // {value} is not a sensitive name
    CHECK(!contains(all, "CANARYKEY"));
    CHECK(!contains(all, "CANARYKEY2"));
    CHECK(!contains(all, "CANARYCOOKIE"));
    CHECK(!contains(all, "CANARYSET"));
    CHECK(!contains(all, "CANARYPROXY"));
    CHECK(!contains(all, "CANARYTOKEN"));
    CHECK(!contains(all, "CANARYSTRUCT"));
  }

  std::fprintf(stderr, "[disabled logging does not call the sink]\n");
  {
    Config cfg = quiet();
    cfg.log.level = log::Level::error;
    Crocket app(cfg);
    if (auto ig = app.ignite(); !ig) return 1;
    clear_lines();
    log::warn("should not appear {value}", std::string("CANARYDROP"));
    log::info("nor this");
    app.flush_logs();
    CHECK(g_sinks.load() == 0);
    CHECK(!contains(lines(), "CANARYDROP"));
  }

  std::fprintf(stderr, "[client options and views]\n");
  {
    Crocket app(quiet());
    if (auto ig = app.ignite(); !ig) return 1;
    clear_lines();
    http::ClientOptions options;
    options.base = "https://user:CANARYUSER@login.example:8443/v1/keys?access_token=CANARYQUERY#frag";
    options.name = "jwks";
    options.ca_file = "/secrets/CANARYCA.pem";
    options.timeout = 2s;
    log::warn("opts {opts}", options);
    auto view = http::for_log(options);
    log::warn("view {view}", view);
    CHECK(view.base == "https://login.example:8443");
    CHECK(view.ca_file);
    CHECK(view.name == "jwks");

    http::CallOptions call;
    call.body = "CANARYBODY";
    call.idempotency_key = "CANARYIDEM";
    call.headers = {{"Authorization", "Bearer CANARYAUTH"}, {"Accept", "application/json"},
                    {"X-Api-Key", "CANARYKEY"}};
    auto logged = http::for_log("https://user:CANARYUSER@pay.example/charge?token=CANARYQUERY", call);
    log::warn("call {call}", logged);
    CHECK(logged.target == "https://pay.example");
    CHECK(logged.body_bytes == std::string("CANARYBODY").size());
    CHECK(logged.idempotency_key);
    CHECK(logged.headers["authorization"] == "[redacted]");
    CHECK(logged.headers["accept"] == "application/json");
    CHECK(logged.headers["x-api-key"] == "[redacted]");
    CHECK(logged.headers["idempotency-key"] == "[redacted]");

    auto path = http::log_target("/v1/cards?pan=CANARYPAN#secret");
    CHECK(path == "/v1/cards");
    CHECK(http::log_endpoint("not a url CANARYUSER") == "[invalid]");

    Response response;
    response.status = 201;
    response.body = "CANARYBODY";
    response.headers.add("set-cookie", "CANARYSET");
    response.headers.add("content-type", "application/json");
    auto shown = http::for_log(response);
    log::warn("response {response}", shown);
    CHECK(shown.status == 201);
    CHECK(shown.body_bytes == std::string("CANARYBODY").size());
    CHECK(shown.headers["set-cookie"] == "[redacted]");
    CHECK(shown.headers["content-type"] == "application/json");
    CHECK(response.headers.get("set-cookie") == std::optional<std::string_view>("CANARYSET"));
    CHECK(response.body == "CANARYBODY");

    app.flush_logs();
    auto all = lines();
    CHECK(!contains(all, "CANARYUSER"));
    CHECK(!contains(all, "CANARYQUERY"));
    CHECK(!contains(all, "CANARYCA"));
    CHECK(!contains(all, "CANARYBODY"));
    CHECK(!contains(all, "CANARYIDEM"));
    CHECK(!contains(all, "CANARYAUTH"));
    CHECK(!contains(all, "CANARYKEY"));
    CHECK(!contains(all, "CANARYSET"));
    CHECK(!contains(all, "CANARYPAN"));
    CHECK(contains(all, "https://login.example:8443"));
    CHECK(contains(all, R"("ca_file":true)"));
    CHECK(contains(all, "application/json"));
  }

  std::fprintf(stderr, "[the wire still carries the header]\n");
  {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(listen_fd, 1) == 0);
    socklen_t len = sizeof addr;
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::string got;
    std::thread server([&] {
      int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) return;
      char buf[2048];
      while (got.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, static_cast<std::size_t>(n));
      }
      std::string resp = "HTTP/1.1 204 X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
      ::close(fd);
    });
    auto base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    http::Client client{{.base = base, .name = "probe", .timeout = 2s, .retry = retry::none, .breaker = breaker::none}};
    http::CallOptions call;
    call.headers = {{"Authorization", "Bearer CANARYAUTH"}};
    call.retry = retry::none;
    call.breaker = breaker::none;
    auto logged = http::for_log("/pay?token=CANARYQUERY", call);
    CHECK(logged.headers["authorization"] == "[redacted]");
    CHECK(logged.target == "/pay");
    auto result = sync_wait(client.get("/pay?token=CANARYQUERY", std::move(call)));
    server.join();
    ::close(listen_fd);
    CHECK(result.has_value());
    CHECK(result->status == 204);
    CHECK(contains(got, "authorization: Bearer CANARYAUTH"));
    CHECK(contains(got, "/pay?token=CANARYQUERY"));
    CHECK(!contains(json::to_string(logged), "CANARYAUTH"));
    CHECK(!contains(json::to_string(logged), "CANARYQUERY"));
  }

  std::fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
