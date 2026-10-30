// A resource server that checks JWTs. Built only when crocket::auth is on.
//
//   CROCKET_AUTH_ISSUER=https://login.example/ \
//   CROCKET_AUTH_AUDIENCE=orders-api \
//   CROCKET_AUTH_JWKS=https://login.example/jwks.json \
//   ./build/crocket_auth_example
//
// Leave CROCKET_AUTH_JWKS unset and the issuer is used for OIDC discovery
// (`{issuer}/.well-known/openid-configuration`). The process still starts if
// that fetch fails; GET /readyz stays not ready until the keys load.
//
//   TOKEN=$(...)   # an access token for that issuer and audience
//   curl -s localhost:8000/me -H "Authorization: Bearer $TOKEN"
//   curl -s localhost:8000/profile -H "Authorization: Bearer $TOKEN"
//   curl -s localhost:8000/admin -H "Authorization: Bearer $TOKEN"   # role admin
//   curl -s localhost:8000/staff -H "Authorization: Bearer $TOKEN"   # admin or ops
//
// HS256 is off unless you also set CROCKET_AUTH_SECRET. from_env() then adds
// HS256. The secret is not taken from the JWKS.

#include <crocket/auth.hpp>
#include <crocket/crocket.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace crocket;

struct UserClaims {
  std::vector<std::string> roles;
  std::optional<std::string> email;
};

namespace api {

[[= http::get("/me")]]
auto me(Auth<> auth) -> std::string {
  std::string line = auth.subject;
  for (auto& role : auth.roles) line += " " + role;
  return line + "\n";
}

[[= http::get("/profile")]]
auto profile(Auth<UserClaims> auth) -> std::string {
  return auth.subject + " " + auth.claims.email.value_or("-") + "\n";
}

[[= http::get("/admin"), = auth::require("admin")]]
auto admin() -> std::string { return "admin\n"; }

[[= http::get("/staff"), = auth::any_of("admin", "ops")]]
auto staff() -> std::string { return "staff\n"; }

}  // namespace api

int main() {
  return build(Config::from_env())
      .manage(auth::Jwt::from_env())
      .attach(Logger{})
      .mount("/", reflect_routes<^^api>())
      .launch();
}
