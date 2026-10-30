#include "internal.hpp"

#include "crocket/json.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>

#include <cstring>
#include <vector>

namespace crocket::auth::detail {
namespace {

bool b64_digit(char c, int& out) {
  if (c >= 'A' && c <= 'Z') return out = c - 'A', true;
  if (c >= 'a' && c <= 'z') return out = c - 'a' + 26, true;
  if (c >= '0' && c <= '9') return out = c - '0' + 52, true;
  if (c == '-') return out = 62, true;
  if (c == '_') return out = 63, true;
  return false;
}

BIGNUM* bn_b64(std::string_view s) {
  std::string raw;
  if (!b64url_decode(s, raw) || raw.empty()) return nullptr;
  return BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), static_cast<int>(raw.size()), nullptr);
}

EVP_PKEY* pkey_from_params(const char* name, OSSL_PARAM* params) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, name, nullptr);
  if (!ctx) return nullptr;
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_fromdata_init(ctx) != 1 || EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) {
    EVP_PKEY_free(pkey);
    pkey = nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

EVP_PKEY* rsa_public(std::string_view n_b64, std::string_view e_b64) {
  BIGNUM* n = bn_b64(n_b64);
  BIGNUM* e = bn_b64(e_b64);
  if (!n || !e) {
    BN_free(n);
    BN_free(e);
    return nullptr;
  }
  OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n);
  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e);
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
  EVP_PKEY* pkey = pkey_from_params("RSA", params);
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  BN_free(n);
  BN_free(e);
  return pkey;
}

EVP_PKEY* ec_public(const char* group, std::string_view x_b64, std::string_view y_b64, std::size_t coord) {
  std::string x, y;
  if (!b64url_decode(x_b64, x) || !b64url_decode(y_b64, y) || x.size() > coord || y.size() > coord) return nullptr;
  std::string point(1 + coord * 2, '\0');
  point[0] = 4;
  std::memcpy(point.data() + 1 + (coord - x.size()), x.data(), x.size());
  std::memcpy(point.data() + 1 + coord + (coord - y.size()), y.data(), y.size());
  OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
  OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0);
  OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
  EVP_PKEY* pkey = pkey_from_params("EC", params);
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  return pkey;
}

EVP_PKEY* okp_public(const char* type, std::string_view x_b64, std::size_t len) {
  std::string x;
  if (!b64url_decode(x_b64, x) || x.size() != len) return nullptr;
  OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
  OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, x.data(), x.size());
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
  EVP_PKEY* pkey = pkey_from_params(type, params);
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  return pkey;
}

const json::Value* field(const json::Value& o, std::string_view k) { return o.find(k); }

std::string_view str_field(const json::Value& o, std::string_view k) {
  auto* v = field(o, k);
  return v && v->is_string() ? std::string_view(v->as_string()) : std::string_view{};
}

bool use_ok(const json::Value& jwk) {
  if (auto use = str_field(jwk, "use"); !use.empty() && use != "sig") return false;
  if (auto* ops = field(jwk, "key_ops"); ops && ops->is_array()) {
    bool verify = false;
    for (auto& op : ops->as_array())
      if (op.is_string() && op.as_string() == "verify") verify = true;
    if (!verify) return false;
  }
  return true;
}

bool verify_rs256(EVP_PKEY* key, std::string_view input, std::string_view sig) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  int ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
           EVP_DigestVerifyUpdate(ctx, input.data(), input.size()) == 1 &&
           EVP_DigestVerifyFinal(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

bool verify_es256(EVP_PKEY* key, std::string_view input, std::string_view sig) {
  if (sig.size() != 64) return false;
  BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(sig.data()), 32, nullptr);
  BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(sig.data()) + 32, 32, nullptr);
  if (!r || !s) {
    BN_free(r);
    BN_free(s);
    return false;
  }
  ECDSA_SIG* ec = ECDSA_SIG_new();
  if (!ec || ECDSA_SIG_set0(ec, r, s) != 1) {
    ECDSA_SIG_free(ec);
    BN_free(r);
    BN_free(s);
    return false;
  }
  int len = i2d_ECDSA_SIG(ec, nullptr);
  if (len <= 0) {
    ECDSA_SIG_free(ec);
    return false;
  }
  std::vector<unsigned char> der(static_cast<std::size_t>(len));
  unsigned char* p = der.data();
  i2d_ECDSA_SIG(ec, &p);
  ECDSA_SIG_free(ec);
  return verify_rs256(key, input, std::string_view(reinterpret_cast<const char*>(der.data()), der.size()));
}

bool verify_eddsa(EVP_PKEY* key, std::string_view input, std::string_view sig) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  int ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
           EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                            reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

}  // namespace

bool b64url_decode(std::string_view in, std::string& out) {
  out.clear();
  if (in.size() > 1 << 20) return false;
  std::string padded(in);
  while (!padded.empty() && padded.back() == '=') padded.pop_back();
  if (padded.size() % 4 == 1) return false;
  out.reserve(padded.size() / 4 * 3 + 3);
  int val = 0, valb = -8;
  for (char c : padded) {
    int d = 0;
    if (!b64_digit(c, d)) return false;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xff));
      valb -= 8;
    }
  }
  return true;
}

bool key_matches(const Key& key, std::string_view alg) {
  if (!key.alg.empty() && key.alg != alg) return false;
  int id = EVP_PKEY_get_base_id(key.pkey);
  if (alg == "RS256") return id == EVP_PKEY_RSA;
  if (alg == "ES256") return id == EVP_PKEY_EC;
  if (alg == "EdDSA") return id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448;
  return false;
}

bool verify_signature(const Key& key, std::string_view alg, std::string_view signing_input, std::string_view sig) {
  if (!key.pkey || sig.empty() || !key_matches(key, alg)) return false;
  if (alg == "RS256") return verify_rs256(key.pkey, signing_input, sig);
  if (alg == "ES256") return verify_es256(key.pkey, signing_input, sig);
  if (alg == "EdDSA") return verify_eddsa(key.pkey, signing_input, sig);
  return false;
}

std::expected<std::shared_ptr<const KeySet>, std::string> parse_jwks(std::string_view json) {
  auto doc = json::parse(json);
  if (!doc) return std::unexpected("jwks is not JSON");
  auto* keys = doc->find("keys");
  if (!doc->is_object() || !keys || !keys->is_array()) return std::unexpected("jwks has no keys array");
  auto set = std::make_shared<KeySet>();
  for (auto& jwk : keys->as_array()) {
    if (!jwk.is_object() || !use_ok(jwk)) continue;
    auto kty = str_field(jwk, "kty");
    auto alg = std::string(str_field(jwk, "alg"));
    if (alg == "none" || alg.starts_with("HS")) continue;
    EVP_PKEY* pkey = nullptr;
    if (kty == "RSA")
      pkey = rsa_public(str_field(jwk, "n"), str_field(jwk, "e"));
    else if (kty == "EC" && str_field(jwk, "crv") == "P-256")
      pkey = ec_public("P-256", str_field(jwk, "x"), str_field(jwk, "y"), 32);
    else if (kty == "OKP" && str_field(jwk, "crv") == "Ed25519")
      pkey = okp_public("ED25519", str_field(jwk, "x"), 32);
    else if (kty == "OKP" && str_field(jwk, "crv") == "Ed448")
      pkey = okp_public("ED448", str_field(jwk, "x"), 57);
    if (!pkey) continue;
    Key key;
    key.kid = std::string(str_field(jwk, "kid"));
    key.alg = std::move(alg);
    key.pkey = pkey;
    set->keys.push_back(std::move(key));
  }
  return set;
}

}  // namespace crocket::auth::detail
