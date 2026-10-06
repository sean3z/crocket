// Async handlers: Task<T> handlers that suspend free their worker, resume on a
// crocket worker with the request's context, and finish the request later.
// In-process; the worker-occupancy check over sockets is in tls_h2.sh.

#include <crocket/crocket.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) {                                                                    \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    ++g_checks;                                                                       \
    auto va_ = (a);                                                                   \
    auto vb_ = (b);                                                                   \
    if (!(va_ == vb_)) {                                                              \
      ++g_failures;                                                               \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (got %s)\n", __FILE__, __LINE__,  \
                   #a, #b, std::format("{}", va_).c_str());                           \
    }                                                                                 \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != hay.npos; }

/// An awaitable from "another library": completes on a thread of its own.
struct ForeignTimer {
  std::chrono::milliseconds d;
  std::thread::id* resumed_on;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) const {
    std::thread([h, d = d, out = resumed_on] {
      std::this_thread::sleep_for(d);
      *out = std::this_thread::get_id();
      h.resume();  // on the library's thread
    }).detach();
  }
  int await_resume() const noexcept { return 7; }
};

std::atomic<int> g_handlers_done{0};

namespace api {

[[= http::get("/sleep/{ms}")]]
auto sleepy(std::uint32_t ms) -> Task<std::string> {
  co_await sleep_for(std::chrono::milliseconds(ms));
  ++g_handlers_done;
  co_return std::format("slept {}", ms);
}

[[= http::get("/context")]]
auto context() -> Task<std::string> {
  log::info("before {step}", 1);
  co_await sleep_for(5ms);
  log::info("after {step}", 2);
  co_return "ok";
}

[[= http::get("/foreign")]]
auto foreign() -> Task<std::string> {
  std::thread::id library_thread;
  int v = co_await ForeignTimer{10ms, &library_thread};
  bool hopped = std::this_thread::get_id() != library_thread;
  log::info("after the foreign await {v}", v);
  co_return std::format("{} {}", v, hopped ? "hopped" : "stayed");
}

[[= http::get("/callback/{mode}")]]
auto with_callback(std::string mode) -> Task<std::string> {
  auto [value, resolve] = callback<std::string>();
  if (mode == "later") {
    std::thread([resolve] {
      std::this_thread::sleep_for(10ms);
      resolve("from another thread");
    }).detach();
  } else if (mode == "now") {
    resolve("already there");
  } else if (mode == "twice") {
    resolve("first");
    resolve("second");
  } else {
    // "dropped": the only Resolve goes away unused
    auto gone = std::move(resolve);
  }
  co_return co_await std::move(value);
}

[[= http::get("/throws/{kind}")]]
auto throws(std::string kind) -> Task<std::string> {
  co_await sleep_for(1ms);
  if (kind == "api") throw ApiError::unprocessable("thing.bad", "bad thing");
  throw std::runtime_error("broke after resuming");
}

[[= http::get("/void")]]
auto void_task() -> Task<> { co_await sleep_for(1ms); }

[[= http::get("/json")]]
auto json_task() -> Task<Json<std::vector<int>>> {
  co_await sleep_for(1ms);
  co_return Json{std::vector{1, 2, 3}};
}

[[= http::get("/nested")]]
auto nested() -> Task<std::string> {
  auto inner = []() -> Task<int> {
    co_await sleep_for(2ms);
    co_return 20;
  };
  int a = co_await inner();
  int b = co_await inner();
  co_return std::to_string(a + b);
}

[[= http::get("/sync")]]
auto sync_task() -> Task<std::string> { co_return "no suspension"; }

}  // namespace api

namespace greeter {
struct Hello {
  std::string name;
};
struct Reply {
  std::string message;
};
[[= grpc::rpc]]
auto greet(Hello h) -> Task<Reply> {
  co_await sleep_for(2ms);
  co_return Reply{"hi " + h.name};
}
}  // namespace greeter

int main() {
  std::mutex mu;
  std::vector<std::string> lines;
  Config cfg;
  cfg.log.sink = [&](std::string_view l) {
    std::lock_guard lk(mu);
    lines.emplace_back(l);
  };
  Crocket app{cfg};
  app.attach(Logger{}).mount("/", reflect_routes<^^api>()).mount("demo", reflect_routes<^^greeter>(), Mode::Grpc);
  LocalClient client(app);
  auto logged = [&](std::string_view needle) {
    std::lock_guard lk(mu);
    for (auto& l : lines)
      if (contains(l, needle)) return l;
    return std::string();
  };

  section("a task that suspends finishes the request when it resumes");
  {
    auto r = client.get("/sleep/20").dispatch();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string("slept 20"));
    CHECK(contains(logged(R"("route":"/sleep/{ms}","handler":"api::sleepy","status":200)"), "duration_ms"));
    CHECK_EQ(client.get("/sync").dispatch().body, std::string("no suspension"));
    CHECK_EQ(client.get("/void").dispatch().status, 204);
    CHECK_EQ(client.get("/json").dispatch().body, std::string("[1,2,3]"));
    CHECK_EQ(client.get("/nested").dispatch().body, std::string("40"));
  }

  section("the request's context survives a resume on another thread");
  {
    auto r = client.get("/context").dispatch();
    auto id = std::string(*r.headers.get("x-request-id"));
    CHECK(contains(logged("before 1"), R"("request_id":")" + id));
    CHECK(contains(logged("after 2"), R"("request_id":")" + id));
    CHECK(contains(logged("after 2"), R"("route":"/context")"));
  }

  section("a foreign awaitable's thread hands back to a worker");
  {
    auto r = client.get("/foreign").dispatch();
    CHECK_EQ(r.body, std::string("7 hopped"));
    CHECK(contains(logged("after the foreign await 7"), R"("route":"/foreign")"));
  }

  section("callback<T>");
  {
    CHECK_EQ(client.get("/callback/later").dispatch().body, std::string("from another thread"));
    CHECK_EQ(client.get("/callback/now").dispatch().body, std::string("already there"));
    CHECK_EQ(client.get("/callback/twice").dispatch().body, std::string("first"));
    auto dropped = client.get("/callback/dropped").dispatch();
    CHECK_EQ(dropped.status, 500);
    CHECK(contains(logged("callback: resolved by nobody"), R"("status":500)"));
  }

  section("exceptions after a resume become the response");
  {
    auto api_err = client.get("/throws/api").dispatch();
    CHECK_EQ(api_err.status, 422);
    CHECK_EQ(api_err.error_code, std::string_view("thing.bad"));
    auto crash = client.get("/throws/other").dispatch();
    CHECK_EQ(crash.status, 500);
    CHECK(!contains(crash.body, "broke after resuming"));
    CHECK(contains(logged("broke after resuming"), R"("code":"internal")"));
  }

  section("async gRPC methods");
  {
    auto r = client.grpc("/demo.Greeter/Greet", proto::encode(greeter::Hello{"ada"})).dispatch();
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, "hi ada"));
  }

  section("many suspended handlers at once");
  {
    int before = g_handlers_done;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> callers;
    std::atomic<int> ok{0};
    for (int i = 0; i < 200; ++i)
      callers.emplace_back([&] {
        if (client.get("/sleep/100").dispatch().body == "slept 100") ++ok;
      });
    for (auto& t : callers) t.join();
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK_EQ(ok.load(), 200);
    CHECK_EQ(g_handlers_done - before, 200);
    // 200 sleeps of 100 ms on a pool of 4 resuming threads: concurrent, not 5 s in a row.
    CHECK(took < 2s);
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
