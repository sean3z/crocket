// Two upstreams, two clients. Built when crocket::client is on.
//
//   BILLING_BASE=http://127.0.0.1:9 PAYMENTS_BASE=http://127.0.0.1:8 \
//     ./build/crocket_client_multi_example
//
// Managed state is one value per type. State<http::Client> is one upstream;
// a second .manage(http::Client) fails at ignite. .with_state<Tag>() moves
// that client into State<http::Client::For<Tag>>.
//
// .name is the Prometheus upstream label. Leave it empty and the host from
// .base is used, so these clients set "billing" and "payments" rather than
// 127.0.0.1.
//
// GET /checkout calls billing GET /invoices/1, then payments POST /charges
// with retries off. Both defaults refuse the connection.

#include <crocket/client.hpp>
#include <crocket/crocket.hpp>

#include <cstdlib>
#include <string>

using namespace crocket;
using namespace std::chrono_literals;

struct Billing;
struct Payments;

namespace api {

[[= http::get("/checkout")]]
auto checkout(State<http::Client::For<Billing>> billing, State<http::Client::For<Payments>> payments)
    -> Task<std::expected<std::string, ApiError>> {
  auto invoice = co_await billing->get("/invoices/1");
  if (!invoice) co_return std::unexpected(invoice.error());
  auto charge = co_await payments->post("/charges", {.body = invoice->body, .retry = retry::none});
  if (!charge) co_return std::unexpected(charge.error());
  co_return std::to_string(charge->status) + "\n";
}

}  // namespace api

int main() {
  const char* billing = std::getenv("BILLING_BASE");
  const char* payments = std::getenv("PAYMENTS_BASE");
  if (!billing || !*billing) billing = "http://127.0.0.1:9";
  if (!payments || !*payments) payments = "http://127.0.0.1:8";
  return build(Config::from_env())
      .manage(http::Client{{
          .base = billing,
          .name = "billing",  // Prometheus upstream label; empty uses the host from .base
          .timeout = 2s,
          .pool = 32,
          .retry = retry::idempotent(3),
          .breaker = breaker::consecutive(5, 30s),
      }}.with_state<Billing>())
      .manage(http::Client{{
          .base = payments,
          .name = "payments",  // Prometheus upstream label; empty uses the host from .base
          .timeout = 2s,
          .pool = 32,
          .retry = retry::idempotent(3),
          .breaker = breaker::consecutive(5, 30s),
      }}.with_state<Payments>())
      .attach(Logger{})
      .mount("/", reflect_routes<^^api>())
      .launch();
}
