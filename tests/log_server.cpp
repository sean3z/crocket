// A small server for tests/logging.sh: request lines, handler lines with
// redacted fields, errors, and numbered requests for ordering checks.

#include <crocket/crocket.hpp>

#include <cstdint>
#include <string>

using namespace crocket;

struct Login {
  std::string user;
  std::string password;
};

namespace logt {

[[= http::get("/ok")]]
auto ok() -> std::string { return "ok"; }

[[= http::get("/seq/{conn}/{n}")]]
auto seq(std::uint32_t conn, std::uint32_t n) -> std::string {
  log::info("seq {conn} {n}", conn, n);
  return std::to_string(n);
}

[[= http::post("/login")]]
auto login(Json<Login> body) -> std::string {
  log::warn("login for {user} with {password}", std::string_view(body->user), std::string_view(body->password));
  log::info("issued {token} for {who}", std::string_view("tok-123"), std::string_view(body->user));
  return "welcome";
}

[[= http::get("/fail")]]
auto fail() -> Result<std::string, ApiError> { return std::unexpected(ApiError::unprocessable("demo.bad", "bad input")); }

[[= http::get("/missing")]]
auto missing() -> std::optional<std::string> { return std::nullopt; }

}  // namespace logt

int main() {
  Config cfg = Config::from_env();
  return build(cfg).attach(Logger{}).attach(Metrics{}).mount("/", reflect_routes<^^logt>()).launch();
}
