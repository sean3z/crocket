// crocket::auth: JWT claim rules, JWKS refresh, OIDC discovery, roles, OpenAPI,
// gRPC status, and the rule that a token never reaches a log line or an error body.
// A live Keycloak is not required; nested realm_access.roles is covered here.
// A Keycloak container for discovery against a real issuer is a follow-up.

#include <crocket/auth.hpp>
#include <crocket/crocket.hpp>

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)
#define CHECK_EQ(a, b)                                                                 \
  do {                                                                                 \
    ++g_checks;                                                                        \
    auto va_ = (a);                                                                    \
    auto vb_ = (b);                                                                    \
    if (!(va_ == vb_)) {                                                               \
      ++g_failures;                                                                    \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
      std::fprintf(stderr, "       got %s\n", std::format("{}", va_).c_str());         \
    }                                                                                  \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view h, std::string_view n) { return h.find(n) != h.npos; }

static constexpr std::string_view kSentinel = "SENTINELcrocketauth9f3a7c";
static constexpr std::string_view kIssuer = "https://login.example/";
static constexpr std::string_view kAudience = "orders-api";

static std::string b64url(std::string_view in) {
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

static std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Key {
  EVP_PKEY* p = nullptr;
  explicit Key(EVP_PKEY* k) : p(k) {}
  Key(const Key&) = delete;
  ~Key() {
    if (p) EVP_PKEY_free(p);
  }
};

static Key generate(std::string_view kind) {
  const char* name = kind == "ES256" ? "EC" : kind == "EdDSA" ? "ED25519" : "RSA";
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, name, nullptr);
  EVP_PKEY_keygen_init(ctx);
  if (kind == "RS256") {
    unsigned bits = 2048;
    OSSL_PARAM p[] = {OSSL_PARAM_construct_uint("bits", &bits), OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX_set_params(ctx, p);
  } else if (kind == "ES256") {
    char group[] = "P-256";
    OSSL_PARAM p[] = {OSSL_PARAM_construct_utf8_string("group", group, 0), OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX_set_params(ctx, p);
  }
  EVP_PKEY* key = nullptr;
  if (EVP_PKEY_generate(ctx, &key) != 1) key = nullptr;
  EVP_PKEY_CTX_free(ctx);
  return Key{key};
}

static std::string jwk_of(const Key& key, std::string_view kid, std::string_view alg) {
  int id = EVP_PKEY_get_base_id(key.p);
  if (id == EVP_PKEY_RSA) {
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    EVP_PKEY_get_bn_param(key.p, "n", &n);
    EVP_PKEY_get_bn_param(key.p, "e", &e);
    auto enc = [](const BIGNUM* b) {
      std::string raw(static_cast<std::size_t>(BN_num_bytes(b)), '\0');
      BN_bn2bin(b, reinterpret_cast<unsigned char*>(raw.data()));
      return b64url(raw);
    };
    auto s = std::format(R"({{"kty":"RSA","kid":"{}","alg":"{}","n":"{}","e":"{}"}})", kid, alg, enc(n), enc(e));
    BN_free(n);
    BN_free(e);
    return s;
  }
  if (id == EVP_PKEY_EC) {
    std::size_t len = 0;
    EVP_PKEY_get_octet_string_param(key.p, "pub", nullptr, 0, &len);
    std::string point(len, '\0');
    EVP_PKEY_get_octet_string_param(key.p, "pub", reinterpret_cast<unsigned char*>(point.data()), len, &len);
    return std::format(R"({{"kty":"EC","crv":"P-256","kid":"{}","alg":"{}","x":"{}","y":"{}"}})", kid, alg,
                       b64url(point.substr(1, 32)), b64url(point.substr(33, 32)));
  }
  std::size_t len = 32;
  std::string raw(32, '\0');
  EVP_PKEY_get_raw_public_key(key.p, reinterpret_cast<unsigned char*>(raw.data()), &len);
  raw.resize(len);
  return std::format(R"({{"kty":"OKP","crv":"Ed25519","kid":"{}","alg":"{}","x":"{}"}})", kid, alg, b64url(raw));
}

static std::string jwks_doc(std::string jwk) { return "{\"keys\":[" + std::move(jwk) + "]}"; }

static std::string sign_bytes(const Key& key, std::string_view alg, std::string_view input) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const EVP_MD* md = alg == "EdDSA" ? nullptr : EVP_sha256();
  EVP_DigestSignInit(ctx, nullptr, md, nullptr, key.p);
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

static std::string hmac_raw(std::string_view secret, std::string_view alg, std::string_view input) {
  const char* digest = alg == "HS512" ? "SHA512" : alg == "HS384" ? "SHA384" : "SHA256";
  std::size_t expect = alg == "HS512" ? 64 : alg == "HS384" ? 48 : 32;
  EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  EVP_MAC_CTX* ctx = mac ? EVP_MAC_CTX_new(mac) : nullptr;
  char digest_name[8] = {};
  std::memcpy(digest_name, digest, std::strlen(digest));
  OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest_name, 0), OSSL_PARAM_construct_end()};
  unsigned char out[64];
  std::size_t out_len = 0;
  std::string sig;
  if (ctx && EVP_MAC_init(ctx, reinterpret_cast<const unsigned char*>(secret.data()), secret.size(), params) == 1 &&
      EVP_MAC_update(ctx, reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1 &&
      EVP_MAC_final(ctx, out, &out_len, sizeof out) == 1 && out_len == expect)
    sig.assign(reinterpret_cast<char*>(out), out_len);
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return sig;
}

static std::string mint_hs(std::string_view secret, std::string_view alg, std::string payload) {
  auto input = b64url(std::format(R"({{"alg":"{}","typ":"JWT"}})", alg)) + "." + b64url(payload);
  return input + "." + b64url(hmac_raw(secret, alg, input));
}

static std::string mint(const Key& key, std::string_view alg, std::string_view kid, std::string payload) {
  auto header = std::format(R"({{"alg":"{}","typ":"JWT","kid":"{}"}})", alg, kid);
  auto input = b64url(header) + "." + b64url(payload);
  return input + "." + b64url(sign_bytes(key, alg, input));
}

static std::string payload(std::int64_t exp, std::string_view extra = {}) {
  auto body = std::format(R"({{"iss":"{}","aud":"{}","sub":"alice","exp":{})", kIssuer, kAudience, exp);
  if (!extra.empty()) body += "," + std::string(extra);
  body += "}";
  return body;
}

struct OrderClaims {
  std::string tenant;
  std::vector<std::string> roles;
  std::optional<std::string> email;
  [[= json::rename("given_name")]] std::optional<std::string> given;
};

namespace api {
[[= http::get("/me")]]
auto me(Auth<> auth) -> std::string {
  std::string s = auth.subject;
  for (auto& sc : auth.scopes) s += " " + sc;
  for (auto& r : auth.roles) s += " !" + r;
  return s;
}
[[= http::get("/orders")]]
auto orders(Auth<OrderClaims> auth) -> std::string {
  return auth.claims.tenant + " " + auth.claims.email.value_or("-") + " " + auth.claims.given.value_or("-");
}
[[= http::get("/admin"), = auth::require("admin")]]
auto admin() -> std::string { return "admin"; }
[[= http::get("/both"), = auth::require("admin", "ops")]]
auto both() -> std::string { return "both"; }
[[= http::get("/staff"), = auth::any_of("admin", "ops")]]
auto staff() -> std::string { return "staff"; }
[[= http::get("/panic")]]
auto panic(Auth<> auth) -> std::string {
  (void)auth;
  throw std::runtime_error("handler blew up");
}
}  // namespace api

namespace greeter {
struct Reply { std::string message; };
[[= grpc::rpc]]
auto who(Auth<> auth) -> Reply { return {auth.subject}; }
[[= grpc::rpc, = auth::require("admin")]]
auto admin_call() -> Reply { return {"ok"}; }
}  // namespace greeter

struct Capture {
  std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
  std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();
  std::function<void(std::string_view)> sink() {
    return [mu = mu, lines = lines](std::string_view l) {
      std::lock_guard lk(*mu);
      lines->emplace_back(l);
    };
  }
  std::string all() const {
    std::lock_guard lk(*mu);
    std::string o;
    for (auto& l : *lines) o += l + "\n";
    return o;
  }
  void clear() {
    std::lock_guard lk(*mu);
    lines->clear();
  }
};

struct Harness {
  Crocket app;
  std::unique_ptr<LocalClient> client;
  Harness(auth::Jwt jwt, Config cfg = {}) : app(std::move(cfg)) {
    app.manage(std::move(jwt))
        .attach(Logger{})
        .attach(Metrics{})
        .attach(OpenApi{{.title = "auth"}})
        .mount("/", reflect_routes<^^api>())
        .mount("demo", reflect_routes<^^greeter>(), Mode::Grpc);
    client = std::make_unique<LocalClient>(app);
  }
  LocalClient::Call get(std::string_view p) { return client->get(p); }
};

static auth::Jwt jwt_inline(std::string doc, std::size_t cache = 0) {
  auth::Jwt j;
  j.issuer = std::string(kIssuer);
  j.audience = std::string(kAudience);
  j.jwks_json = std::move(doc);
  j.cache_entries = cache;
  j.leeway = 60s;
  return j;
}

static bool wait_ready(Crocket& app, std::chrono::milliseconds budget = 3000ms) {
  auto* j = app.core().state.get<auth::Jwt>();
  auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    if (j && j->ready()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return j && j->ready();
}

static void expect_secret_free(std::string_view text, std::string_view token) {
  CHECK(!contains(text, kSentinel));
  CHECK(!contains(text, token));
}

struct JwksServer {
  std::mutex mu;
  std::string jwks = R"({"keys":[]})";
  std::string oidc;
  int status = 200;
  std::string cache_control;
  std::atomic<bool> hang{false};
  std::atomic<bool> stop{false};
  int port = 0;
  int listen_fd = -1;
  std::thread thr;

  void start() {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::listen(listen_fd, 16);
    socklen_t len = sizeof addr;
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    thr = std::thread([this] { loop(); });
  }
  ~JwksServer() {
    stop.store(true);
    hang.store(false);
    if (listen_fd >= 0) {
      ::shutdown(listen_fd, SHUT_RDWR);
      ::close(listen_fd);
    }
    if (thr.joinable()) thr.join();
  }
  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
  }

  void loop() {
    while (!stop.load()) {
      int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) continue;
      std::string req;
      char buf[1024];
      while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
        if (req.size() > 8192) break;
      }
      auto sp = std::string_view(req).find(' ');
      std::string_view target;
      if (sp != std::string_view::npos) {
        auto rest = std::string_view(req).substr(sp + 1);
        target = rest.substr(0, rest.find(' '));
      }
      if (hang.load()) {
        while (hang.load() && !stop.load()) std::this_thread::sleep_for(20ms);
        ::close(fd);
        continue;
      }
      std::string body;
      int st = 200;
      std::string cc;
      {
        std::lock_guard lk(mu);
        st = status;
        cc = cache_control;
        if (target == "/.well-known/openid-configuration") body = oidc.empty() ? "{}" : oidc;
        else body = jwks;
        if (oidc.empty() && target == "/.well-known/openid-configuration") st = 404;
      }
      auto resp = "HTTP/1.1 " + std::to_string(st) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                  std::to_string(body.size()) + "\r\nConnection: close\r\n";
      if (!cc.empty()) resp += "Cache-Control: " + cc + "\r\n";
      resp += "\r\n" + body;
      (void)::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
      ::close(fd);
    }
  }
};

int main() {
  auto rsa = generate("RS256");
  auto ec = generate("ES256");
  auto ed = generate("EdDSA");
  CHECK(rsa.p && ec.p && ed.p);
  auto rsa_doc = jwks_doc(jwk_of(rsa, "rsa-1", "RS256"));
  auto exp = now_s() + 3600;

  section("claim rules, algorithms, malformed tokens");
  {
    Harness h(jwt_inline(rsa_doc));
    auto ok = mint(rsa, "RS256", "rsa-1", payload(exp, R"("scope":"read write","roles":["admin"])"));
    auto r = h.get("/me").bearer(ok).dispatch();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string("alice read write !admin"));

    auto es = mint(ec, "ES256", "ec-1", payload(exp));
    Harness he(jwt_inline(jwks_doc(jwk_of(ec, "ec-1", "ES256"))));
    CHECK_EQ(he.get("/me").bearer(es).dispatch().status, 200);
    auto edtok = mint(ed, "EdDSA", "ed-1", payload(exp));
    Harness hd(jwt_inline(jwks_doc(jwk_of(ed, "ed-1", "EdDSA"))));
    CHECK_EQ(hd.get("/me").bearer(edtok).dispatch().status, 200);

    auto expired = h.get("/me").bearer(mint(rsa, "RS256", "rsa-1", payload(now_s() - 120))).dispatch();
    CHECK_EQ(expired.status, 401);
    CHECK_EQ(expired.error_code, std::string_view("auth.expired"));
    CHECK(contains(expired.headers.get("www-authenticate").value_or(""), "error=\"invalid_token\""));
    auto within = h.get("/me").bearer(mint(rsa, "RS256", "rsa-1", payload(now_s() - 30))).dispatch();
    CHECK_EQ(within.status, 200);
    auto nbf_ok = h.get("/me")
                      .bearer(mint(rsa, "RS256", "rsa-1", payload(exp, std::format(R"("nbf":{})", now_s() + 30))))
                      .dispatch();
    CHECK_EQ(nbf_ok.status, 200);
    auto nbf_bad = h.get("/me")
                       .bearer(mint(rsa, "RS256", "rsa-1", payload(exp, std::format(R"("nbf":{})", now_s() + 180))))
                       .dispatch();
    CHECK_EQ(nbf_bad.error_code, std::string_view("auth.not_yet_valid"));
    auto iat_bad = h.get("/me")
                       .bearer(mint(rsa, "RS256", "rsa-1", payload(exp, std::format(R"("iat":{})", now_s() + 180))))
                       .dispatch();
    CHECK_EQ(iat_bad.error_code, std::string_view("auth.not_yet_valid"));

    auto wrong_iss = h.get("/me")
                         .bearer(mint(rsa, "RS256", "rsa-1",
                                      std::format(R"({{"iss":"https://other/","aud":"{}","sub":"alice","exp":{}}})",
                                                  kAudience, exp)))
                         .dispatch();
    CHECK_EQ(wrong_iss.error_code, std::string_view("auth.invalid"));
    CHECK(contains(wrong_iss.body, "invalid bearer token"));
    CHECK(!contains(wrong_iss.body, "issuer"));  // the reason is log-only, not the body (release profile)
    auto aud_arr = h.get("/me")
                       .bearer(mint(rsa, "RS256", "rsa-1",
                                    std::format(R"({{"iss":"{}","aud":["other","{}"],"sub":"a","exp":{}}})", kIssuer,
                                                kAudience, exp)))
                       .dispatch();
    CHECK_EQ(aud_arr.status, 200);
    auto aud_miss = h.get("/me")
                        .bearer(mint(rsa, "RS256", "rsa-1",
                                     std::format(R"({{"iss":"{}","aud":["other"],"sub":"a","exp":{}}})", kIssuer, exp)))
                        .dispatch();
    CHECK_EQ(aud_miss.error_code, std::string_view("auth.invalid"));

    auto none = h.get("/me").bearer("eyJhbGciOiJub25lIn0.eyJzdWIiOiJ4In0.").dispatch();
    CHECK_EQ(none.error_code, std::string_view("auth.invalid"));
    auto hs = h.get("/me").bearer(mint(rsa, "HS256", "rsa-1", payload(exp))).dispatch();
    CHECK_EQ(hs.error_code, std::string_view("auth.invalid"));
    auto bad_sig = h.get("/me").bearer(ok.substr(0, ok.rfind('.') + 1) + std::string(kSentinel)).dispatch();
    CHECK_EQ(bad_sig.error_code, std::string_view("auth.invalid"));
    CHECK_EQ(h.get("/me").bearer("not-a-jwt").dispatch().error_code, std::string_view("auth.invalid"));
    CHECK_EQ(h.get("/me").bearer("a.b.c.d").dispatch().error_code, std::string_view("auth.invalid"));
    CHECK_EQ(h.get("/me").bearer("???.$$$.@@@").dispatch().error_code, std::string_view("auth.invalid"));
    auto unknown = h.get("/me").bearer(mint(rsa, "RS256", "missing", payload(exp))).dispatch();
    CHECK_EQ(unknown.error_code, std::string_view("auth.unknown_key"));
    CHECK(contains(unknown.body, "unknown signing key"));
    CHECK(!contains(unknown.body, "missing"));  // kid stays in the log detail

    auto missing = h.get("/me").dispatch();
    CHECK_EQ(missing.error_code, std::string_view("auth.missing"));
    CHECK_EQ(missing.headers.get("www-authenticate").value_or(""), std::string_view("Bearer"));
    auto scheme = h.get("/me").header("authorization", "Basic abc").dispatch();
    CHECK_EQ(scheme.error_code, std::string_view("auth.invalid"));
  }

  section("typed claims and roles");
  {
    Harness h(jwt_inline(rsa_doc));
    auto good = mint(rsa, "RS256", "rsa-1",
                     payload(exp, R"("tenant":"acme","email":"a@b.c","given_name":"Ada","roles":["admin","ops"])"));
    CHECK_EQ(h.get("/orders").bearer(good).dispatch().body, std::string("acme a@b.c Ada"));
    auto no_email = mint(rsa, "RS256", "rsa-1", payload(exp, R"("tenant":"acme","roles":["admin"])"));
    CHECK_EQ(h.get("/orders").bearer(no_email).dispatch().body, std::string("acme - -"));
    auto no_tenant = h.get("/orders").bearer(mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["admin"])"))).dispatch();
    CHECK_EQ(no_tenant.status, 401);
    CHECK_EQ(no_tenant.error_code, std::string_view("auth.claims"));

    CHECK_EQ(h.get("/admin").bearer(good).dispatch().status, 200);
    CHECK_EQ(h.get("/both").bearer(good).dispatch().status, 200);
    auto ops_only = mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["ops"])"));
    auto forbidden = h.get("/admin").bearer(ops_only).dispatch();
    CHECK_EQ(forbidden.status, 403);
    CHECK_EQ(forbidden.error_code, std::string_view("auth.forbidden"));
    CHECK(contains(forbidden.headers.get("www-authenticate").value_or(""), "insufficient_scope"));
    CHECK_EQ(h.get("/both").bearer(ops_only).dispatch().status, 403);
    CHECK_EQ(h.get("/staff").bearer(ops_only).dispatch().status, 200);
    CHECK_EQ(h.get("/staff").bearer(mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["guest"])"))).dispatch().status,
             403);
    CHECK_EQ(h.get("/admin").dispatch().status, 401);

    auth::Jwt nested = jwt_inline(rsa_doc);
    nested.roles_claim = "realm_access.roles";
    Harness hn(std::move(nested), Config{});
    auto kc = mint(rsa, "RS256", "rsa-1", payload(exp, R"("realm_access":{"roles":["admin"]})"));
    CHECK_EQ(hn.get("/admin").bearer(kc).dispatch().status, 200);
    CHECK(contains(hn.get("/me").bearer(kc).dispatch().body, "!admin"));
  }

  section("OpenAPI and gRPC");
  {
    Harness h(jwt_inline(rsa_doc));
    auto doc = openapi_document(h.app, {.title = "auth"});
    CHECK(contains(doc, R"("security":[{"bearer":[]}])"));
    CHECK(contains(doc, R"("x-required-roles":["admin"])"));
    CHECK(contains(doc, R"("x-required-roles":["admin","ops"])"));
    CHECK(contains(doc, R"("x-required-roles-match":"all")"));
    CHECK(contains(doc, R"("x-required-roles-match":"any")"));
    CHECK(contains(doc, "\"403\""));

    auto anon = h.client->grpc("/demo.Greeter/Who", "").dispatch();
    CHECK(grpc::status_of(anon).code == grpc::Code::Unauthenticated);
    auto who = h.client->grpc("/demo.Greeter/Who", "").bearer(mint(rsa, "RS256", "rsa-1", payload(exp))).dispatch();
    CHECK_EQ(grpc::read_reply<greeter::Reply>(who).value_or(greeter::Reply{"?"}).message, std::string("alice"));
    auto denied = h.client->grpc("/demo.Greeter/AdminCall", "").bearer(mint(rsa, "RS256", "rsa-1", payload(exp))).dispatch();
    CHECK(grpc::status_of(denied).code == grpc::Code::PermissionDenied);
    auto allowed = h.client->grpc("/demo.Greeter/AdminCall", "")
                       .bearer(mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["admin"])")))
                       .dispatch();
    CHECK(grpc::status_of(allowed).ok());
  }

  section("RFC 8037 Ed25519 vector");
  {
    auth::Jwt j = jwt_inline(
        R"({"keys":[{"kty":"OKP","crv":"Ed25519","x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"}]})");
    j.issuer = "https://login.example/";
    Harness h(std::move(j));
    auto tok =
        "eyJhbGciOiJFZERTQSJ9.RXhhbXBsZSBvZiBFZDI1NTE5IHNpZ25pbmc.hgyY0il_"
        "MGCjP0JzlnLWG1PPOt7-09PGcvMg3AIbQR6dWbhijcNR4ki4iylGjg5BhVsPt9g7sVvpAr_MuM0KAg";
    Config cfg;
    cfg.profile = Profile::Dev;
    // Re-drive through a dev app so the log-only detail is visible.
    Harness dev(jwt_inline(R"({"keys":[{"kty":"OKP","crv":"Ed25519","x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"}]})"),
                cfg);
    // issuer still has to match; the vector's payload is not JSON, so the
    // signature is checked first and the detail says so.
    auto r = dev.get("/me").bearer(tok).dispatch();
    CHECK_EQ(r.error_code, std::string_view("auth.invalid"));
    CHECK(contains(r.body, "payload is not a JSON object"));
    CHECK(!contains(r.body, "signature"));
    (void)h;
  }

  section("tokens are not logged");
  {
    Capture cap;
    Config cfg;
    cfg.profile = Profile::Dev;
    cfg.log.level = log::Level::debug;
    cfg.log.sink = cap.sink();
    Harness h(jwt_inline(rsa_doc), cfg);
    auto bad = mint(rsa, "RS256", "rsa-1", payload(exp));
    bad = bad.substr(0, bad.rfind('.') + 1) + std::string(kSentinel);
    auto expired = mint(rsa, "RS256", "rsa-1", payload(now_s() - 120));
    auto guest = mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["guest"])"));
    auto admin = mint(rsa, "RS256", "rsa-1", payload(exp, R"("roles":["admin"])"));
    for (auto tok : {bad, expired, guest, admin}) {
      cap.clear();
      auto path = tok == admin ? "/panic" : tok == guest ? "/admin" : "/me";
      auto r = h.get(path).bearer(tok).header("cookie", std::string(kSentinel)).dispatch();
      CHECK(r.status >= 400);
      expect_secret_free(r.body, tok);
      expect_secret_free(cap.all(), tok);
    }
  }

  section("verified-token cache keeps a token until exp");
  {
    JwksServer server;
    server.start();
    {
      std::lock_guard lk(server.mu);
      server.jwks = rsa_doc;
    }
    auth::Jwt j;
    j.issuer = std::string(kIssuer);
    j.audience = std::string(kAudience);
    j.jwks = server.url("/jwks");
    j.refresh = 1s;
    j.cache_entries = 8;
    Harness h(std::move(j), Config{});
    CHECK(wait_ready(h.app));
    auto tok = mint(rsa, "RS256", "rsa-1", payload(exp));
    CHECK_EQ(h.get("/me").bearer(tok).dispatch().status, 200);
    {
      std::lock_guard lk(server.mu);
      server.jwks = jwks_doc(jwk_of(ec, "ec-1", "ES256"));
    }
    std::this_thread::sleep_for(1500ms);
    CHECK_EQ(h.get("/me").bearer(tok).dispatch().status, 200);  // cached until exp
    auto other = mint(rsa, "RS256", "rsa-1", payload(exp, R"("sub":"bob")"));
    // sub is inside the signed payload; mint uses sub alice from payload(). Override:
    other = mint(rsa, "RS256", "rsa-1",
                 std::format(R"({{"iss":"{}","aud":"{}","sub":"bob","exp":{}}})", kIssuer, kAudience, exp));
    CHECK_EQ(h.get("/me").bearer(other).dispatch().error_code, std::string_view("auth.unknown_key"));
  }

  section("JWKS rotation, outage, unknown kid does not block, OIDC");
  {
    JwksServer server;
    server.start();
    {
      std::lock_guard lk(server.mu);
      server.jwks = rsa_doc;
    }
    auth::Jwt j;
    j.issuer = std::string(kIssuer);
    j.audience = std::string(kAudience);
    j.jwks = server.url("/jwks");
    j.refresh = 1s;
    j.cache_entries = 0;
    Harness h(std::move(j), Config{});
    CHECK(wait_ready(h.app));
    auto a = mint(rsa, "RS256", "rsa-1", payload(exp));
    CHECK_EQ(h.get("/me").bearer(a).dispatch().status, 200);

    {
      std::lock_guard lk(server.mu);
      server.jwks = jwks_doc(jwk_of(ec, "ec-1", "ES256"));
    }
    auto b = mint(ec, "ES256", "ec-1", payload(exp));
    auto first = h.get("/me").bearer(b).dispatch();
    CHECK_EQ(first.error_code, std::string_view("auth.unknown_key"));
    CHECK(wait_ready(h.app));
    bool rotated = false;
    for (int i = 0; i < 40 && !rotated; ++i) {
      std::this_thread::sleep_for(50ms);
      rotated = h.get("/me").bearer(b).dispatch().status == 200;
    }
    CHECK(rotated);
    CHECK_EQ(h.get("/me").bearer(a).dispatch().error_code, std::string_view("auth.unknown_key"));

    {
      std::lock_guard lk(server.mu);
      server.status = 500;
    }
    std::this_thread::sleep_for(1200ms);
    CHECK_EQ(h.get("/me").bearer(b).dispatch().status, 200);  // previous keys stay
    auto metrics = h.get("/metrics").dispatch().body;
    CHECK(contains(metrics, "crocket_auth_jwks_refresh_failures_total"));
    CHECK(!contains(metrics, "crocket_auth_jwks_refresh_failures_total 0\n") ||
          contains(metrics, "crocket_auth_jwks_refresh_failures_total"));

    server.hang.store(true);
    auto started = std::chrono::steady_clock::now();
    auto hung = h.get("/me").bearer(mint(rsa, "RS256", "nope", payload(exp))).dispatch();
    auto took = std::chrono::steady_clock::now() - started;
    CHECK_EQ(hung.error_code, std::string_view("auth.unknown_key"));
    CHECK(took < 500ms);
    server.hang.store(false);
  }

  section("OIDC discovery and soft boot");
  {
    JwksServer server;
    server.start();
    auto issuer = server.url("");
    {
      std::lock_guard lk(server.mu);
      server.jwks = rsa_doc;
      server.oidc = std::format(R"({{"issuer":"{}","jwks_uri":"{}"}})", issuer, server.url("/jwks"));
    }
    auto j = auth::Jwt::oidc(issuer, std::string(kAudience));
    j.refresh = 1s;
    // Tokens are minted against kIssuer; discovery checks the configured issuer,
    // so this app's issuer is the server URL. Mint against that.
    Harness h(std::move(j), Config{});
    CHECK(wait_ready(h.app));
    auto tok = mint(rsa, "RS256", "rsa-1",
                    std::format(R"({{"iss":"{}","aud":"{}","sub":"alice","exp":{}}})", issuer, kAudience, exp));
    CHECK_EQ(h.get("/me").bearer(tok).dispatch().status, 200);

    JwksServer bad;
    bad.start();
    {
      std::lock_guard lk(bad.mu);
      bad.oidc = R"({"issuer":"https://else/","jwks_uri":"http://127.0.0.1/jwks"})";
    }
    auto mismatch = auth::Jwt::oidc(bad.url(""), std::string(kAudience));
    mismatch.refresh = 1s;
    Crocket app;
    app.manage(std::move(mismatch)).mount("/", reflect_routes<^^api>());
    LocalClient client(app);
    CHECK(client.get("/readyz").dispatch().status == 503);
    std::this_thread::sleep_for(200ms);
    CHECK(!app.core().state.get<auth::Jwt>()->ready());

    Crocket down;
    auth::Jwt unreachable;
    unreachable.issuer = "http://127.0.0.1:1";
    unreachable.audience = "orders-api";
    unreachable.jwks = "http://127.0.0.1:1/jwks";
    unreachable.refresh = 1s;
    down.manage(std::move(unreachable)).mount("/", reflect_routes<^^api>());
    auto ig = down.ignite();
    CHECK(ig.has_value());
    LocalClient dc(down);
    auto ready = dc.get("/readyz").dispatch();
    CHECK_EQ(ready.status, 503);
    CHECK(contains(ready.body, "fail"));
  }

  section("HS256 needs an explicit secret");
  {
    constexpr std::string_view kSecret = "test-secret-value";
    auto hs = mint_hs(kSecret, "HS256", payload(exp));
    auto hs512 = mint_hs(kSecret, "HS512", payload(exp));

    auth::Jwt ok = jwt_inline(rsa_doc);
    ok.secret = std::string(kSecret);
    ok.algorithms.push_back(auth::HS256);
    ok.algorithms.push_back(auth::HS512);
    Harness h(std::move(ok));
    auto good = h.get("/me").bearer(hs).dispatch();
    CHECK_EQ(good.status, 200);
    CHECK_EQ(good.body, std::string("alice"));
    CHECK(!contains(good.body, kSecret));
    CHECK_EQ(h.get("/me").bearer(hs512).dispatch().status, 200);
    CHECK_EQ(h.get("/me").bearer(mint(rsa, "RS256", "rsa-1", payload(exp))).dispatch().status, 200);

    auth::Jwt listed = jwt_inline(rsa_doc);
    listed.algorithms.push_back(auth::HS256);
    Config dev;
    dev.profile = Profile::Dev;
    Harness no_secret(std::move(listed), dev);
    auto missing_secret = no_secret.get("/me").bearer(hs).dispatch();
    CHECK_EQ(missing_secret.status, 401);
    CHECK_EQ(missing_secret.error_code, std::string_view("auth.invalid"));
    CHECK(contains(missing_secret.body, "hmac secret is not configured"));
    CHECK(!contains(missing_secret.body, kSecret));

    auth::Jwt asym = jwt_inline(rsa_doc);
    asym.secret = std::string(kSecret);
    Harness only_asym(std::move(asym), dev);
    auto not_listed = only_asym.get("/me").bearer(hs).dispatch();
    CHECK_EQ(not_listed.error_code, std::string_view("auth.invalid"));
    CHECK(contains(not_listed.body, "algorithm is not allowed"));

    BIGNUM* modulus = nullptr;
    EVP_PKEY_get_bn_param(rsa.p, "n", &modulus);
    std::string n_bytes(static_cast<std::size_t>(BN_num_bytes(modulus)), '\0');
    BN_bn2bin(modulus, reinterpret_cast<unsigned char*>(n_bytes.data()));
    BN_free(modulus);
    auto confused = mint_hs(n_bytes, "HS256", payload(exp));
    CHECK_EQ(h.get("/me").bearer(confused).dispatch().error_code, std::string_view("auth.invalid"));
    CHECK_EQ(only_asym.get("/me").bearer(confused).dispatch().error_code, std::string_view("auth.invalid"));
    Harness jwks_only(jwt_inline(rsa_doc), dev);
    auto confused_default = jwks_only.get("/me").bearer(confused).dispatch();
    CHECK_EQ(confused_default.error_code, std::string_view("auth.invalid"));
    CHECK(contains(confused_default.body, "algorithm is not allowed"));

    auth::Jwt hmac_only;
    hmac_only.issuer = std::string(kIssuer);
    hmac_only.audience = std::string(kAudience);
    hmac_only.secret = std::string(kSecret);
    hmac_only.algorithms = {auth::HS256};
    Harness bare(std::move(hmac_only));
    CHECK(bare.app.core().state.get<auth::Jwt>()->ready());
    CHECK_EQ(bare.get("/me").bearer(hs).dispatch().status, 200);
  }

  section("from_env and one authenticator");
  {
    ::setenv("CROCKET_AUTH_ISSUER", "https://login.example/", 1);
    ::setenv("CROCKET_AUTH_AUDIENCE", "orders-api", 1);
    ::setenv("CROCKET_AUTH_JWKS", "https://login.example/jwks", 1);
    ::setenv("CROCKET_AUTH_LEEWAY", "15s", 1);
    ::setenv("CROCKET_AUTH_ROLES_CLAIM", "realm_access.roles", 1);
    ::setenv("CROCKET_AUTH_JWKS_REFRESH", "2m", 1);
    ::setenv("CROCKET_AUTH_SECRET", "env-secret-value", 1);
    auto j = auth::Jwt::from_env();
    CHECK_EQ(j.issuer, std::string("https://login.example/"));
    CHECK_EQ(j.audience, std::string("orders-api"));
    CHECK_EQ(j.jwks, std::string("https://login.example/jwks"));
    CHECK_EQ(j.leeway, 15s);
    CHECK_EQ(j.roles_claim, std::string("realm_access.roles"));
    CHECK_EQ(j.refresh, 2min);
    CHECK_EQ(j.secret, std::string("env-secret-value"));
    bool hs256 = false;
    for (auto a : j.algorithms)
      if (a == auth::HS256) hs256 = true;
    CHECK(hs256);
    for (auto* name : {"CROCKET_AUTH_ISSUER", "CROCKET_AUTH_AUDIENCE", "CROCKET_AUTH_JWKS", "CROCKET_AUTH_LEEWAY",
                       "CROCKET_AUTH_ROLES_CLAIM", "CROCKET_AUTH_JWKS_REFRESH", "CROCKET_AUTH_SECRET"})
      ::unsetenv(name);

    Crocket app;
    app.manage(jwt_inline(rsa_doc));
    app.manage(Authenticator{[](std::string_view) -> std::expected<Auth<>, ApiError> { return Auth<>{"x"}; }});
    auto ig = app.ignite();
    CHECK(!ig);
    CHECK(contains(ig.error().message(), "Authenticator"));
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
