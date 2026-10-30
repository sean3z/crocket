// In-process cost of a me(Auth<>) route. Same row format as bench/micro.cpp.
//
//   ./dev bench auth
//   ./build/crocket_bench_auth
//
// auth.off is the pipeline with no Auth parameter. auth.hit.* repeats one
// token so the per-worker cache skips the signature. auth.miss.* sets
// cache_entries to 0, so every request verifies. Keys are generated in
// process (RS256 2048, ES256 P-256, Ed25519) and served from an inline JWKS,
// so nothing waits on the network.

#include <crocket/auth.hpp>
#include <crocket/crocket.hpp>

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

static std::atomic<std::uint64_t> g_allocs{0};
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace crocket;

namespace api {
[[= http::get("/off")]]
auto off() -> std::string { return "alice\n"; }

[[= http::get("/me")]]
auto me(Auth<> auth) -> std::string { return auth.subject + "\n"; }
}  // namespace api

namespace {

std::string b64url(std::string_view in) {
  static constexpr char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned val = 0;
  int valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(t[(val >> valb) & 0x3f]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(t[((val << 8) >> (valb + 8)) & 0x3f]);
  return out;
}

EVP_PKEY* generate(const char* name, const char* group, unsigned bits) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, name, nullptr);
  if (!ctx || EVP_PKEY_keygen_init(ctx) != 1) return nullptr;
  if (group) {
    char buf[16] = {};
    std::memcpy(buf, group, std::strlen(group));
    OSSL_PARAM p[] = {OSSL_PARAM_construct_utf8_string("group", buf, 0), OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX_set_params(ctx, p);
  } else if (bits) {
    OSSL_PARAM p[] = {OSSL_PARAM_construct_uint("bits", &bits), OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX_set_params(ctx, p);
  }
  EVP_PKEY* key = nullptr;
  EVP_PKEY_generate(ctx, &key);
  EVP_PKEY_CTX_free(ctx);
  return key;
}

std::string jwk(EVP_PKEY* key, std::string_view kid, std::string_view alg) {
  int id = EVP_PKEY_get_base_id(key);
  if (id == EVP_PKEY_RSA) {
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    EVP_PKEY_get_bn_param(key, "n", &n);
    EVP_PKEY_get_bn_param(key, "e", &e);
    auto enc = [](const BIGNUM* b) {
      std::string raw(static_cast<std::size_t>(BN_num_bytes(b)), '\0');
      BN_bn2bin(b, reinterpret_cast<unsigned char*>(raw.data()));
      return b64url(raw);
    };
    auto s = std::string("{\"kty\":\"RSA\",\"kid\":\"") + std::string(kid) + "\",\"alg\":\"" + std::string(alg) +
             "\",\"n\":\"" + enc(n) + "\",\"e\":\"" + enc(e) + "\"}";
    BN_free(n);
    BN_free(e);
    return s;
  }
  if (id == EVP_PKEY_EC) {
    std::size_t len = 0;
    EVP_PKEY_get_octet_string_param(key, "pub", nullptr, 0, &len);
    std::string point(len, '\0');
    EVP_PKEY_get_octet_string_param(key, "pub", reinterpret_cast<unsigned char*>(point.data()), len, &len);
    return "{\"kty\":\"EC\",\"crv\":\"P-256\",\"kid\":\"" + std::string(kid) + "\",\"alg\":\"" + std::string(alg) +
           "\",\"x\":\"" + b64url(point.substr(1, 32)) + "\",\"y\":\"" + b64url(point.substr(33, 32)) + "\"}";
  }
  std::size_t len = 32;
  std::string raw(32, '\0');
  EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(raw.data()), &len);
  raw.resize(len);
  return "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"" + std::string(kid) + "\",\"alg\":\"" + std::string(alg) +
         "\",\"x\":\"" + b64url(raw) + "\"}";
}

std::string sign(EVP_PKEY* key, std::string_view alg, std::string_view input) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const EVP_MD* md = alg == "EdDSA" ? nullptr : EVP_sha256();
  EVP_DigestSignInit(ctx, nullptr, md, nullptr, key);
  std::string sig;
  if (alg == "EdDSA") {
    std::size_t len = 0;
    EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(input.data()), input.size());
    sig.assign(len, '\0');
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                   reinterpret_cast<const unsigned char*>(input.data()), input.size());
    sig.resize(len);
  } else {
    EVP_DigestSignUpdate(ctx, input.data(), input.size());
    std::size_t len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &len);
    sig.assign(len, '\0');
    EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len);
    sig.resize(len);
    if (alg == "ES256") {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(sig.data());
      ECDSA_SIG* ec = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()));
      const BIGNUM* r = nullptr;
      const BIGNUM* s = nullptr;
      ECDSA_SIG_get0(ec, &r, &s);
      std::string raw(64, '\0');
      BN_bn2binpad(r, reinterpret_cast<unsigned char*>(raw.data()), 32);
      BN_bn2binpad(s, reinterpret_cast<unsigned char*>(raw.data()) + 32, 32);
      ECDSA_SIG_free(ec);
      sig = std::move(raw);
    }
  }
  EVP_MD_CTX_free(ctx);
  return sig;
}

std::string mint(EVP_PKEY* key, std::string_view alg, std::string_view kid, std::string_view issuer, std::int64_t exp) {
  auto header = std::string("{\"alg\":\"") + std::string(alg) + "\",\"typ\":\"JWT\",\"kid\":\"" + std::string(kid) + "\"}";
  auto payload = std::string("{\"iss\":\"") + std::string(issuer) + "\",\"aud\":\"bench\",\"sub\":\"alice\",\"exp\":" +
                 std::to_string(exp) + "}";
  auto input = b64url(header) + "." + b64url(payload);
  return input + "." + b64url(sign(key, alg, input));
}

template <class F>
void measure(const char* name, F op) {
  for (int i = 0; i < 200; ++i) op();
  std::uint64_t n = 0;
  auto a0 = g_allocs.load();
  auto t0 = std::chrono::steady_clock::now();
  auto until = t0 + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < until) {
    for (int i = 0; i < 64; ++i) op();
    n += 64;
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
  double allocs = double(g_allocs.load() - a0) / double(n);
  std::printf("%s\tns_per_op\t%.1f\tlower\n%s\tallocs_per_op\t%.2f\tlower\n", name, ns, name, allocs);
}

auth::Jwt make_jwt(std::string jwks, std::size_t cache) {
  auth::Jwt j;
  j.issuer = "https://bench.example/";
  j.audience = "bench";
  j.jwks_json = std::move(jwks);
  j.cache_entries = cache;
  return j;
}

}  // namespace

int main() {
  EVP_PKEY* rsa = generate("RSA", nullptr, 2048);
  EVP_PKEY* ec = generate("EC", "P-256", 0);
  EVP_PKEY* ed = generate("ED25519", nullptr, 0);
  if (!rsa || !ec || !ed) {
    std::fprintf(stderr, "key generation failed\n");
    return 1;
  }
  auto doc = std::string("{\"keys\":[") + jwk(rsa, "rsa", "RS256") + "," + jwk(ec, "ec", "ES256") + "," +
             jwk(ed, "ed", "EdDSA") + "]}";
  auto exp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
             86400;
  auto rs = mint(rsa, "RS256", "rsa", "https://bench.example/", exp);
  auto es = mint(ec, "ES256", "ec", "https://bench.example/", exp);
  auto edd = mint(ed, "EdDSA", "ed", "https://bench.example/", exp);
  EVP_PKEY_free(rsa);
  EVP_PKEY_free(ec);
  EVP_PKEY_free(ed);

  Config cfg;
  cfg.log.sink = [](std::string_view) {};
  Crocket hit_app{cfg};
  hit_app.manage(make_jwt(doc, 1024)).mount("/", reflect_routes<^^api>());
  LocalClient hit(hit_app);
  Crocket miss_app{cfg};
  miss_app.manage(make_jwt(doc, 0)).mount("/", reflect_routes<^^api>());
  LocalClient miss(miss_app);

  auto expect_ok = [](Response res, const char* label) {
    if (res.status != 200) {
      std::fprintf(stderr, "%s failed: status %d body %s\n", label, res.status, res.body.c_str());
      std::exit(1);
    }
  };
  expect_ok(hit.get("/off").dispatch(), "auth.off");
  expect_ok(hit.get("/me").bearer(rs).dispatch(), "auth.hit.rs256");
  expect_ok(hit.get("/me").bearer(es).dispatch(), "auth.hit.es256");
  expect_ok(hit.get("/me").bearer(edd).dispatch(), "auth.hit.eddsa");
  expect_ok(miss.get("/me").bearer(rs).dispatch(), "auth.miss.rs256");
  expect_ok(miss.get("/me").bearer(es).dispatch(), "auth.miss.es256");
  expect_ok(miss.get("/me").bearer(edd).dispatch(), "auth.miss.eddsa");

  std::fprintf(stderr,
               "auth.off: no Auth parameter. auth.hit: one token, cache of 1024. "
               "auth.miss: cache_entries=0, signature on every request.\n");
  volatile std::size_t sink = 0;
  measure("auth.off", [&] { sink += hit.get("/off").dispatch().body.size(); });
  measure("auth.hit.rs256", [&] { sink += hit.get("/me").bearer(rs).dispatch().body.size(); });
  measure("auth.hit.es256", [&] { sink += hit.get("/me").bearer(es).dispatch().body.size(); });
  measure("auth.hit.eddsa", [&] { sink += hit.get("/me").bearer(edd).dispatch().body.size(); });
  measure("auth.miss.rs256", [&] { sink += miss.get("/me").bearer(rs).dispatch().body.size(); });
  measure("auth.miss.es256", [&] { sink += miss.get("/me").bearer(es).dispatch().body.size(); });
  measure("auth.miss.eddsa", [&] { sink += miss.get("/me").bearer(edd).dispatch().body.size(); });
  return sink == 0 ? 1 : 0;
}
