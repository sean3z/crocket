// Outbound HTTP. Built when crocket::client is on.
//
//   CROCKET_UPSTREAM=http://127.0.0.1:9 ./build/crocket_client_example
//
// GET /invoices/{id} calls GET {CROCKET_UPSTREAM}/invoices/{id}.
// POST /charges sends "{}" and does not retry (retry::none).
// The default upstream refuses connections, so those routes answer 503
// upstream.unavailable until CROCKET_UPSTREAM points at something.

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <cstdlib>
#include <string>

using namespace crocket;
using namespace std::chrono_literals;

struct Invoice {
  std::string id;
  int cents = 0;
};

namespace api {

[[= http::get("/invoices/{id}")]]
auto invoice(std::string id, State<http::Client> billing) -> Task<std::expected<Json<Invoice>, ApiError>> {
  auto r = co_await billing->get("/invoices/" + id);
  if (!r) co_return std::unexpected(r.error());
  if (r->status != 200)
    co_return std::unexpected(ApiError{502, "upstream.status", "billing rejected the invoice", {}});
  auto parsed = http::json<Invoice>(*r);
  if (!parsed) co_return std::unexpected(parsed.error());
  co_return Json<Invoice>{std::move(*parsed)};
}

[[= http::post("/charges")]]
auto charge(State<http::Client> billing) -> Task<std::expected<std::string, ApiError>> {
  auto r = co_await billing->post("/charges", {.body = "{}", .retry = retry::none});
  if (!r) co_return std::unexpected(r.error());
  co_return std::to_string(r->status) + "\n";
}

}  // namespace api

int main() {
  const char* base = std::getenv("CROCKET_UPSTREAM");
  if (!base || !*base) base = "http://127.0.0.1:9";
  return build(Config::from_env())
      .manage(http::Client{{
          .base = base,
          .timeout = 2s,
          .pool = 64,
          .retry = retry::idempotent(3),
          .breaker = breaker::consecutive(5, 30s),
      }})
      .attach(Logger{})
      .mount("/", reflect_routes<^^api>())
      .launch();
}
