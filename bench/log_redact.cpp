// Cost of diagnostic redaction. No sockets and no stdio inside the timed loop.
//
//   cmake --build <build> --target crocket_bench_log
//   ./crocket_bench_log
//
// Rows are name, ns/op, and a note. "log_*" lines go through log::warn with a
// sink that discards the line. "for_log_*" only builds the safe view.
// Disabled warn returns before formatting.

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>

using namespace crocket;
using namespace std::chrono_literals;

namespace {

std::atomic<std::uint64_t> g_bytes{0};

void discard(std::string_view line) { g_bytes.fetch_add(line.size(), std::memory_order_relaxed); }

template <class F>
void row(const char* name, F op) {
  for (int i = 0; i < 2000; ++i) op();
  constexpr int kBatch = 128;
  std::uint64_t n = 0;
  auto until = std::chrono::steady_clock::now() + 400ms;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < until) {
    for (int i = 0; i < kBatch; ++i) op();
    n += kBatch;
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
  std::printf("%-22s %10.1f ns/op\n", name, ns);
}

Headers sample(std::size_t n) {
  Headers h;
  for (std::size_t i = 0; i < n; ++i) {
    if (i % 7 == 0) h.add("Authorization", "Bearer canary-token-value");
    else if (i % 7 == 1) h.add("X-Api-Key", "canary-api-key");
    else if (i % 7 == 2) h.add("Cookie", "sid=canary-cookie");
    else h.add("x-field-" + std::to_string(i), "value-" + std::to_string(i));
  }
  return h;
}

}  // namespace

int main() {
  Config info;
  info.log.level = log::Level::info;
  info.log.sink = discard;
  Crocket mid(info);
  if (!mid.ignite()) return 1;
  row("debug_when_info", [] { log::debug("skipped {n}", 1); });

  Config on;
  on.log.level = log::Level::debug;
  on.log.sink = discard;
  Crocket loud(on);
  if (!loud.ignite()) return 1;

  std::map<std::string, std::string> small{{"Authorization", "Bearer canary"},
                                           {"X-Api-Key", "canary-key"},
                                           {"Accept", "application/json"},
                                           {"X-Request-Id", "req-1"},
                                           {"Cookie", "sid=canary"},
                                           {"Content-Type", "application/json"}};
  std::map<std::string, std::string> large = small;
  for (int i = 0; i < 40; ++i) large.emplace("x-field-" + std::to_string(i), "value-" + std::to_string(i));

  http::ClientOptions options;
  options.base = "https://user:canary@login.example:8443/v1/keys?access_token=canary";
  options.name = "jwks";
  options.ca_file = "/secrets/ca.pem";
  http::CallOptions call;
  call.body = std::string(2048, 'x');
  call.idempotency_key = "idem-canary";
  call.headers = {{"Authorization", "Bearer canary"}, {"Accept", "application/json"}};
  Response response;
  response.status = 200;
  response.body = std::string(4096, 'y');
  response.headers.add("set-cookie", "sid=canary");
  response.headers.add("content-type", "application/json");
  auto headers_small = sample(6);
  auto headers_large = sample(48);

  std::printf("discarded_bytes_before\t%llu\n", static_cast<unsigned long long>(g_bytes.load()));
  row("warn_string", [] { log::warn("hello {name}", std::string("ada")); });
  row("warn_map_6", [&] { log::warn("h {h}", small); });
  row("warn_map_46", [&] { log::warn("h {h}", large); });
  row("warn_options", [&] { log::warn("o {o}", options); });
  row("for_log_headers_6", [&] { auto v = http::for_log(headers_small); g_bytes.fetch_add(v.size(), std::memory_order_relaxed); });
  row("for_log_headers_48", [&] { auto v = http::for_log(headers_large); g_bytes.fetch_add(v.size(), std::memory_order_relaxed); });
  row("for_log_options", [&] { auto v = http::for_log(options); g_bytes.fetch_add(v.base.size(), std::memory_order_relaxed); });
  row("for_log_call", [&] { auto v = http::for_log("/pay?token=canary", call); g_bytes.fetch_add(v.body_bytes, std::memory_order_relaxed); });
  row("for_log_response", [&] { auto v = http::for_log(response); g_bytes.fetch_add(v.body_bytes, std::memory_order_relaxed); });
  row("log_for_log_response", [&] { log::warn("r {r}", http::for_log(response)); });
  std::printf("discarded_bytes_after\t%llu\n", static_cast<unsigned long long>(g_bytes.load()));
  return 0;
}
