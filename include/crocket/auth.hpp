#pragma once
// Opt-in JWT / OIDC verifier. Not included by crocket.hpp: a program that
// links only crocket::crocket does not get this code.
//
//   #include <crocket/auth.hpp>
//   app.manage(auth::Jwt{
//       .issuer = "https://login.example.com/",
//       .audience = "orders-api",
//       .jwks = "https://login.example.com/.well-known/jwks.json",
//   });
//
// auth::Jwt is an Authenticator. Managing it and a custom Authenticator
// both fails at ignite. Key lookup never waits on the network: an unknown
// kid fails the request immediately and refreshes in the background.

#include "crocket/app.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace crocket::auth {

/// Signature algorithms. The default `.algorithms` is RS256, ES256 and EdDSA.
/// HS256, HS384 and HS512 are off until listed here and `.secret` is set.
/// `none` cannot be enabled.
enum class Alg : std::uint8_t { RS256, ES256, EdDSA, HS256, HS384, HS512 };
inline constexpr Alg RS256 = Alg::RS256;
inline constexpr Alg ES256 = Alg::ES256;
inline constexpr Alg EdDSA = Alg::EdDSA;
inline constexpr Alg HS256 = Alg::HS256;
inline constexpr Alg HS384 = Alg::HS384;
inline constexpr Alg HS512 = Alg::HS512;

/// Resource-server JWT check. Aggregate, so `.issuer = ...` works.
/// EdDSA verifies Ed25519 and Ed448 keys. The verified-token cache is per
/// worker and keeps a token until its `exp` (a revoked token stays accepted
/// until then; there is no introspection).
struct Jwt {
  struct State;

  std::string issuer;
  std::string audience;
  /// JWKS URL. Empty: fetch `{issuer}/.well-known/openid-configuration` and
  /// use its `jwks_uri`. The document's `issuer` must match `issuer` exactly.
  std::string jwks;
  std::vector<Alg> algorithms = {Alg::RS256, Alg::ES256, Alg::EdDSA};
  /// HMAC key. Empty rejects HS256, HS384 and HS512 even if they are listed in
  /// `.algorithms`. Never taken from a JWKS key (that is the alg-confusion
  /// attack: an RSA or EC public key reused as an HMAC secret).
  std::string secret;
  std::chrono::milliseconds leeway{60'000};
  /// Claim that holds roles: "roles", "groups", "scope", or a dotted path
  /// such as "realm_access.roles". An array of strings, or one space-separated
  /// string. `Auth::scopes` is always the top-level "scope" claim, separate
  /// from this.
  std::string roles_claim = "roles";
  std::chrono::milliseconds refresh{600'000};
  std::string subject_claim = "sub";
  /// Entries in the per-worker cache of verified tokens. 0 turns the cache off.
  std::size_t cache_entries = 1024;
  /// JWKS document instead of a URL. Nothing is fetched. For tests, and for a
  /// process that already has the document.
  std::string jwks_json;
  /// PEM bundle of extra CAs for the JWKS fetch. Empty uses the system trust store.
  std::string tls_ca_file;
  /// Verifier state. Leave unset; manage() fills it.
  mutable std::shared_ptr<State> state;

  /// OIDC discovery. `jwks` stays empty until the document is fetched.
  static Jwt oidc(std::string issuer, std::string audience) {
    Jwt j;
    j.issuer = std::move(issuer);
    j.audience = std::move(audience);
    return j;
  }

  /// `cfg.auth` from Config. An empty issuer with no JWKS never becomes ready.
  static Jwt from(const AuthConfig& cfg);
  /// `Config::from_env().auth` (CROCKET_AUTH_ISSUER and the rest).
  static Jwt from_env();

  /// Starts the key refresh, once. Called by the conversion below.
  std::shared_ptr<State> activate() const;
  /// True after the first JWKS document (and, for OIDC, discovery) has loaded.
  /// A failed fetch keeps the previous keys and leaves this false until one succeeds.
  /// An HMAC-only verifier (a secret, no JWKS URL) is ready without a fetch.
  [[nodiscard]] bool ready() const;

  operator Authenticator() const;
};

}  // namespace crocket::auth

namespace crocket::detail {
template <>
struct manage_as<crocket::auth::Jwt> {
  using type = crocket::Authenticator;
};
}  // namespace crocket::detail
