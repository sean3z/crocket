#pragma once
// Shared by the auth translation units. Not a public header.

#include "crocket/auth.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace crocket::auth::detail {

struct Fetched {
  int status = 0;
  std::string body;
  std::chrono::seconds max_age{0};  // 0: Cache-Control did not say
};

/// One HTTPS (or loopback HTTP) GET. Times out. Does not follow more than
/// three redirects. `ca_file` empty: the default trust store.
/// Stopgap: replace with crocket::client (Roadmap #37 / RESILIENCE, on
/// h2o_httpclient). Do not call h2o directly from crocket_auth.
std::expected<Fetched, std::string> http_get(std::string_view url, std::string_view ca_file);

struct Key {
  std::string kid;
  std::string alg;  // from the JWK, or empty
  EVP_PKEY* pkey = nullptr;
  Key() = default;
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  Key(Key&& o) noexcept : kid(std::move(o.kid)), alg(std::move(o.alg)), pkey(o.pkey) { o.pkey = nullptr; }
  Key& operator=(Key&&) = delete;
  ~Key() {
    if (pkey) EVP_PKEY_free(pkey);
  }
};

struct KeySet {
  std::vector<Key> keys;
};

std::expected<std::shared_ptr<const KeySet>, std::string> parse_jwks(std::string_view json);

/// The jwks_uri. Fails when `issuer` in the document is not `expected_issuer`.
std::expected<std::string, std::string> parse_discovery(std::string_view json, std::string_view expected_issuer);

bool b64url_decode(std::string_view in, std::string& out);

/// True when `sig` is a valid `alg` signature over `signing_input` for `key`.
bool verify_signature(const Key& key, std::string_view alg, std::string_view signing_input, std::string_view sig);

/// HMAC-SHA256/384/512 over `signing_input` with `secret`. Does not read a JWK.
bool verify_hmac(std::string_view secret, std::string_view alg, std::string_view signing_input, std::string_view sig);

/// Whether this parsed key can verify `alg` (RS256 / ES256 / EdDSA).
bool key_matches(const Key& key, std::string_view alg);

}  // namespace crocket::auth::detail
