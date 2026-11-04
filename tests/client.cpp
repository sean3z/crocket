// crocket::client: policy decisions in-process, and HTTP against a toy server.
// The designated-initializer call shapes are compiled in shapes().

#include "client_policy.hpp"

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace crocket;
using namespace std::chrono_literals;
using client_detail::Circuit;
using client_detail::FailKind;
using client_detail::RetryBudget;
using client_detail::backoff_cap;
using client_detail::effective_timeout;
using client_detail::full_jitter;
using client_detail::is_idempotent;
using client_detail::parse_retry_after;
using client_detail::should_retry;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)
#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    ++g_checks;                                                                     \
    auto va_ = (a);                                                                 \
    auto vb_ = (b);                                                                 \
    if (!(va_ == vb_)) {                                                            \
      ++g_failures;                                                                 \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                               \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

namespace {

class Toy {
 public:
  explicit Toy(std::function<std::string(const std::string&)> handle) : handle_(std::move(handle)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) std::perror("bind");
    socklen_t len = sizeof addr;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    if (listen(fd_, 64) != 0) std::perror("listen");
    thread_ = std::thread([this] { loop(); });
  }
  ~Toy() {
    stop_.store(true);
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (thread_.joinable()) thread_.join();
  }
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  void loop() {
    while (!stop_.load()) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(fd_, &fds);
      timeval tv{0, 50'000};
      if (select(fd_ + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
      int conn = ::accept(fd_, nullptr, nullptr);
      if (conn < 0) continue;
      std::string req;
      char buf[2048];
      while (!stop_.load() && req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(conn, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, std::size_t(n));
        if (req.size() > (1 << 20)) break;
      }
      auto res = handle_ ? handle_(req) : std::string{};
      if (!res.empty()) (void)::send(conn, res.data(), res.size(), MSG_NOSIGNAL);
      ::close(conn);
    }
  }
  int fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::function<std::string(const std::string&)> handle_;
  std::thread thread_;
};

std::string http_response(int status, std::string_view body, std::string_view extra = {}) {
  std::string o = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nConnection: close\r\n";
  if (!extra.empty()) {
    o.append(extra);
    if (!extra.ends_with("\r\n")) o += "\r\n";
  }
  o += "\r\n";
  o.append(body);
  return o;
}

http::Client make_client(const Toy& toy, http::ClientOptions options = {}) {
  options.base = toy.base();
  if (options.name.empty()) options.name = "toy";
  return http::Client{std::move(options)};
}

void shapes() {
  http::Client billing{{
      .base = "http://127.0.0.1:9",
      .timeout = 2s,
      .pool = 64,
      .retry = retry::idempotent(3),
      .breaker = breaker::consecutive(5, 30s),
  }};
  auto once = billing.post("/charges", {.body = "{}", .retry = retry::none});
  auto quiet = billing.post("/charges", {.body = "{}", .retry = retry::none, .breaker = breaker::none});
  (void)once;
  (void)quiet;
}

}  // namespace

namespace api {

[[= http::get("/proxy")]]
auto proxy(State<http::Client> billing) -> Task<std::string> {
  auto r = co_await billing->get("/hang");
  if (!r) co_return std::string(r.error().code);
  co_return "status " + std::to_string(r->status);
}

[[= http::get("/who")]]
auto who(State<http::Client> billing) -> Task<std::string> {
  auto r = co_await billing->get("/echo");
  if (!r) co_return std::string(r.error().code);
  co_return r->body;
}

[[= http::get("/ready")]]
auto ready(State<http::Client> billing) -> Task<std::string> {
  auto r = co_await billing->get("/ready");
  if (!r) co_return std::string(r.error().code);
  co_return r->body;
}

}  // namespace api

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  section("policy");
  CHECK(is_idempotent(http::Method::Get));
  CHECK(is_idempotent(http::Method::Put));
  CHECK(is_idempotent(http::Method::Delete));
  CHECK(!is_idempotent(http::Method::Post));
  CHECK(!is_idempotent(http::Method::Patch));
  CHECK(should_retry(retry::idempotent(3), http::Method::Get, false, FailKind::Http, 503, 1, 4));
  CHECK(!should_retry(retry::idempotent(3), http::Method::Post, false, FailKind::Http, 503, 1, 4));
  CHECK(should_retry(retry::idempotent(3), http::Method::Post, true, FailKind::Connect, 0, 1, 4));
  CHECK(!should_retry(retry::none, http::Method::Get, false, FailKind::Http, 503, 1, 1));
  CHECK(!should_retry(retry::idempotent(3), http::Method::Get, false, FailKind::Http, 500, 1, 4));
  CHECK(should_retry(retry::idempotent(3), http::Method::Get, false, FailKind::Http, 429, 1, 4));
  CHECK(!should_retry(retry::idempotent(3), http::Method::Get, false, FailKind::TooLarge, 200, 1, 4));
  CHECK_EQ(backoff_cap(retry::idempotent(3), 0).count(), 20);
  CHECK_EQ(backoff_cap(retry::idempotent(3), 1).count(), 40);
  auto cap = backoff_cap(retry::idempotent(3), 1);
  for (int i = 0; i < 20; ++i) {
    auto delay = full_jitter(cap);
    CHECK(delay.count() >= 0 && delay.count() <= cap.count());
  }
  CHECK_EQ(backoff_cap(retry::Policy{.initial = 800ms, .max_delay = 1s}, 5).count(), 1000);

  auto now = Clock::now();
  auto left = effective_timeout(now + 100ms, 5s);
  CHECK(left.count() > 50 && left.count() <= 100);
  CHECK_EQ(effective_timeout(Clock::time_point::max(), 2s).count(), 2000);
  CHECK_EQ(effective_timeout(now + 10ms, 5s).count(), 0);

  CHECK(parse_retry_after("0").has_value());
  CHECK_EQ(parse_retry_after("2")->count(), 2000);
  CHECK(!parse_retry_after("soon"));
  CHECK(parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT")->count() == 0);

  {
    RetryBudget budget(0.2, 0);
    for (int i = 0; i < 100; ++i) budget.note_call();
    int n = 0;
    while (budget.allow_retry()) ++n;
    CHECK_EQ(n, 20);
  }
  {
    RetryBudget budget(0.0, 3);
    budget.note_call();
    CHECK(budget.allow_retry());
    CHECK(budget.allow_retry());
    CHECK(budget.allow_retry());
    CHECK(!budget.allow_retry());
  }
  {
    auto t0 = Clock::time_point(Clock::duration{1'000'000'000});
    Circuit circuit(breaker::consecutive(2, 30s));
    CHECK(circuit.allow(t0));
    CHECK(!circuit.on_failure(t0));
    CHECK(circuit.allow(t0));
    CHECK(circuit.on_failure(t0));
    CHECK(!circuit.allow(t0));
    CHECK_EQ(circuit.state(t0), Circuit::Open);
    CHECK(circuit.allow(t0 + 30s));
    CHECK_EQ(circuit.state(t0 + 30s), Circuit::HalfOpen);
    CHECK(circuit.on_failure(t0 + 30s));
    CHECK(!circuit.allow(t0 + 30s + 30s));
    CHECK(circuit.allow(t0 + 30s + 60s));
  }
  {
    auto t0 = Clock::time_point(Clock::duration{1'000'000'000});
    Circuit circuit(breaker::consecutive(1, 10s));
    CHECK(circuit.on_failure(t0));
    CHECK(circuit.allow(t0 + 10s));
    CHECK(circuit.on_success(t0 + 10s));
    CHECK_EQ(circuit.state(t0 + 10s), Circuit::Closed);
  }
  {
    auto t0 = Clock::time_point(Clock::duration{1'000'000'000});
    Circuit circuit(breaker::rate(0.5, 4, 10s));  // min_samples is half the window
    CHECK(!circuit.on_failure(t0));
    CHECK(circuit.on_failure(t0));
    CHECK(!circuit.allow(t0));
  }

  section("designated init");
  shapes();

  section("get, post, json");
  {
    std::atomic<int> hits{0};
    std::string first;
    std::mutex mu;
    Toy toy([&](const std::string& req) {
      int n = hits.fetch_add(1);
      {
        std::lock_guard lk(mu);
        if (first.empty()) first = req;
      }
      if (contains(req, "POST /charges")) return http_response(201, "made");
      if (n < 2) return http_response(503, "no");
      return http_response(200, "{\"id\":\"inv-1\",\"cents\":40}");
    });
    auto client = make_client(toy, {.name = "billing", .retry = retry::idempotent(3), .breaker = breaker::none});
    auto got = sync_wait(client.get("/invoices/inv-1"));
    CHECK(got.has_value());
    if (got) {
      CHECK_EQ(got->status, 200);
      struct Invoice {
        std::string id;
        int cents = 0;
      };
      auto invoice = http::json<Invoice>(*got);
      CHECK(invoice.has_value());
      if (invoice) {
        CHECK(invoice->id == "inv-1");
        CHECK_EQ(invoice->cents, 40);
      }
    }
    CHECK(hits.load() >= 3);
    {
      std::lock_guard lk(mu);
      CHECK(contains(first, "user-agent: crocket"));
    }
    hits.store(0);
    auto posted = sync_wait(client.post("/charges", {.body = "{}", .retry = retry::none}));
    CHECK(posted.has_value());
    if (posted) {
      CHECK_EQ(posted->status, 201);
      CHECK(posted->body == "made");
    }
    CHECK_EQ(hits.load(), 1);
  }

  section("idempotency and retry-after");
  {
    std::atomic<int> posts{0};
    Toy toy([&](const std::string& req) {
      if (!contains(req, "POST ")) return http_response(200, "ok");
      int n = posts.fetch_add(1);
      bool keyed = contains(req, "idempotency-key:");
      if (!keyed) return http_response(503, "no");
      if (n == 0) return http_response(503, "later");
      return http_response(200, "ok");
    });
    auto client = make_client(toy, {.retry = retry::idempotent(3), .breaker = breaker::none});
    auto bare = sync_wait(client.post("/pay", {.body = "{}"}));
    CHECK(bare.has_value());
    if (bare) CHECK_EQ(bare->status, 503);
    CHECK_EQ(posts.load(), 1);
    posts.store(0);
    auto keyed = sync_wait(client.post("/pay", {.body = "{}", .idempotency_key = "k-1"}));
    CHECK(keyed.has_value());
    if (keyed) CHECK_EQ(keyed->status, 200);
    CHECK_EQ(posts.load(), 2);
  }
  {
    std::atomic<int> hits{0};
    Toy toy([&](const std::string&) {
      if (hits.fetch_add(1) == 0) return http_response(429, "slow", "Retry-After: 0\r\n");
      return http_response(200, "ok");
    });
    auto client = make_client(toy, {.retry = retry::idempotent(2), .breaker = breaker::none});
    auto got = sync_wait(client.get("/limited"));
    CHECK(got.has_value());
    if (got) CHECK_EQ(got->status, 200);
    CHECK_EQ(hits.load(), 2);
  }

  section("budget");
  {
    std::atomic<int> hits{0};
    Toy toy([&](const std::string&) {
      hits.fetch_add(1);
      return http_response(503, "no");
    });
    retry::Policy policy = retry::idempotent(5);
    policy.budget = 0;
    policy.budget_min = 0;
    auto client = make_client(toy, {.retry = policy, .breaker = breaker::none});
    auto got = sync_wait(client.get("/"));
    CHECK(got.has_value());
    if (got) CHECK_EQ(got->status, 503);
    CHECK_EQ(hits.load(), 1);
  }

  section("circuit");
  {
    std::atomic<int> hits{0};
    Toy toy([&](const std::string&) {
      hits.fetch_add(1);
      return http_response(503, "down");
    });
    auto client = make_client(toy, {.name = "billing", .retry = retry::none, .breaker = breaker::consecutive(2, 30s)});
    auto first = sync_wait(client.get("/"));
    auto second = sync_wait(client.get("/"));
    auto third = sync_wait(client.get("/"));
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(!third.has_value());
    if (!third) {
      CHECK(third.error().code == "upstream.circuit_open");
      CHECK_EQ(third.error().status, 503);
    }
    CHECK_EQ(hits.load(), 2);
  }

  section("timeout and refused");
  {
    std::atomic<bool> release{false};
    Toy toy([&](const std::string&) {
      for (int i = 0; i < 100 && !release.load(); ++i) std::this_thread::sleep_for(20ms);
      return http_response(200, "late");
    });
    auto client = make_client(toy, {.timeout = 300ms, .connect_timeout = 300ms, .retry = retry::none, .breaker = breaker::none});
    auto t0 = Clock::now();
    auto got = sync_wait(client.get("/hang"));
    release.store(true);
    CHECK(!got.has_value());
    if (!got) {
      CHECK(got.error().code == "upstream.timeout");
      CHECK_EQ(got.error().status, 504);
    }
    CHECK(Clock::now() - t0 < 2s);
  }
  {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    auto port = ntohs(addr.sin_port);
    ::close(fd);
    http::Client client{{.base = "http://127.0.0.1:" + std::to_string(port),
                         .name = "closed",
                         .timeout = 500ms,
                         .retry = retry::none,
                         .breaker = breaker::none}};
    auto got = sync_wait(client.get("/"));
    CHECK(!got.has_value());
    if (!got) {
      CHECK(got.error().code == "upstream.unavailable");
      CHECK_EQ(got.error().status, 503);
    }
  }

  section("deadline, request id, metrics");
  {
    std::mutex mu;
    std::string last;
    std::atomic<bool> release{false};
    Toy toy([&](const std::string& req) {
      {
        std::lock_guard lk(mu);
        last = req;
      }
      if (contains(req, "GET /hang ")) {
        for (int i = 0; i < 150 && !release.load(); ++i) std::this_thread::sleep_for(20ms);
        return http_response(200, "late");
      }
      if (contains(req, "x-request-id: req-123")) return http_response(200, "id");
      if (contains(req, "x-request-id:")) return http_response(200, "invented");
      return http_response(200, "none");
    });
    Config cfg;
    cfg.log.sink = [](std::string_view) {};
    cfg.timeouts.request = 500ms;
    Crocket app(cfg);
    app.manage(http::Client{{
           .base = toy.base(),
           .name = "billing",
           .timeout = 5s,
           .retry = retry::none,
           .breaker = breaker::none,
       }})
        .attach(Metrics{})
        .mount("/", reflect_routes<^^api>());
    LocalClient local(app);
    auto t0 = Clock::now();
    auto hung = local.get("/proxy").dispatch();
    release.store(true);
    CHECK_EQ(hung.status, 200);
    CHECK(hung.body == "upstream.timeout");
    CHECK(Clock::now() - t0 < 2s);

    auto named = local.get("/who").header("x-request-id", "req-123").header("traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01").dispatch();
    CHECK_EQ(named.status, 200);
    CHECK(named.body == "id");
    {
      std::lock_guard lk(mu);
      CHECK(!contains(last, "traceparent"));
      CHECK(contains(last, "x-request-id: req-123"));
    }
    auto plain = local.get("/who").dispatch();
    CHECK(plain.body == "none");

    auto ok = local.get("/ready").dispatch();
    CHECK(ok.body == "none");
    auto metrics = local.get("/metrics").dispatch();
    CHECK(contains(metrics.body, "crocket_client_requests_total"));
    CHECK(contains(metrics.body, "crocket_client_request_duration_seconds"));
    CHECK(contains(metrics.body, "crocket_client_breaker_state"));
    CHECK(contains(metrics.body, "upstream=\"billing\""));
    CHECK(!contains(metrics.body, "127.0.0.1"));
  }

  std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
