#include "internal.hpp"

#include "crocket/fairings.hpp"
#include "crocket/json.hpp"
#include "crocket/log.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace crocket::auth {
namespace {

std::atomic<std::uint64_t> g_refresh_failures{0};

void render_metrics(std::string& o) {
  o += "# HELP crocket_auth_jwks_refresh_failures_total JWKS or OIDC discovery fetches that failed.\n";
  o += "# TYPE crocket_auth_jwks_refresh_failures_total counter\n";
  o += "crocket_auth_jwks_refresh_failures_total " + std::to_string(g_refresh_failures.load()) + "\n";
}

std::once_flag g_metrics_once;

json::ReadOptions token_json() {
  json::ReadOptions o;
  o.max_depth = 8;
  o.max_string_bytes = 8192;
  o.max_object_members = 64;
  o.max_array_elements = 256;
  return o;
}

std::unexpected<ApiError> invalid(std::string detail) {
  ApiError e = ApiError::unauthorized("auth.invalid", "invalid bearer token");
  e.detail = std::move(detail);
  return std::unexpected(std::move(e));
}

std::unexpected<ApiError> coded(std::string_view code, std::string message, std::string detail) {
  ApiError e = ApiError::unauthorized(code, std::move(message));
  e.detail = std::move(detail);
  return std::unexpected(std::move(e));
}

const json::Value* walk(const json::Value& root, std::string_view path) {
  const json::Value* cur = &root;
  while (!path.empty()) {
    auto dot = path.find('.');
    auto key = path.substr(0, dot);
    if (key.empty() || !cur->is_object()) return nullptr;
    cur = cur->find(key);
    if (!cur) return nullptr;
    if (dot == std::string_view::npos) break;
    path.remove_prefix(dot + 1);
  }
  return cur;
}

void split_words(std::string_view s, std::vector<std::string>& out) {
  while (!s.empty()) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    if (s.empty()) break;
    auto sp = s.find_first_of(" \t");
    out.emplace_back(s.substr(0, sp));
    if (sp == std::string_view::npos) break;
    s.remove_prefix(sp + 1);
  }
}

void strings_of(const json::Value* v, std::vector<std::string>& out) {
  if (!v) return;
  if (v->is_string()) split_words(v->as_string(), out);
  else if (v->is_array())
    for (auto& e : v->as_array())
      if (e.is_string() && !e.as_string().empty()) out.push_back(e.as_string());
}

std::optional<std::int64_t> json_seconds(const json::Value* v) {
  if (!v || !v->is_number()) return std::nullopt;
  if (v->is_int()) return v->as_int();
  double d = v->as_double();
  if (!std::isfinite(d) || d < 0 || d > 1e12) return std::nullopt;
  return static_cast<std::int64_t>(d);
}

struct CacheEntry {
  std::uint64_t rep = 0;
  unsigned char sha[32]{};
  std::int64_t exp = 0;
  std::string claims;
  std::string subject;
  std::vector<std::string> scopes;
  std::vector<std::string> roles;
};

struct TokenCache {
  std::vector<CacheEntry> entries;
  std::size_t next = 0;
};

thread_local TokenCache g_cache;

bool sha256(std::string_view token, unsigned char out[32]) {
  unsigned int n = 0;
  return EVP_Digest(token.data(), token.size(), out, &n, EVP_sha256(), nullptr) == 1 && n == 32;
}

}  // namespace

struct Jwt::State {
  struct Cfg {
    std::string issuer;
    std::string audience;
    std::string jwks;
    std::string roles_claim = "roles";
    std::string subject_claim = "sub";
    std::string tls_ca_file;
    std::chrono::milliseconds leeway{60'000};
    std::chrono::milliseconds refresh{600'000};
    bool rs = true;
    bool es = true;
    bool ed = true;
    std::size_t cache_entries = 1024;
    std::uint64_t id = 0;
  } cfg;

  std::atomic<std::shared_ptr<const detail::KeySet>> keys;
  std::atomic<bool> loaded{false};
  std::chrono::milliseconds interval{600'000};

  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  bool wake = false;
  std::thread thread;
  std::atomic<std::int64_t> last_nudge{0};

  explicit State(const Jwt& src);
  ~State();
  void loop();
  bool fetch();
  void note_failure(std::string_view why);
  void nudge();
  [[nodiscard]] bool ready() const { return loaded.load(std::memory_order_acquire); }
  [[nodiscard]] bool alg_ok(std::string_view alg) const;
  std::expected<Authenticator::Verified, ApiError> verify(std::string_view token);
  std::expected<Authenticator::Verified, ApiError> check_claims(std::string payload_json);
};

Jwt::State::State(const Jwt& src) {
  static std::atomic<std::uint64_t> next_id{1};
  cfg.id = next_id.fetch_add(1);
  cfg.issuer = src.issuer;
  cfg.audience = src.audience;
  cfg.jwks = src.jwks;
  cfg.roles_claim = src.roles_claim.empty() ? "roles" : src.roles_claim;
  cfg.subject_claim = src.subject_claim.empty() ? "sub" : src.subject_claim;
  cfg.tls_ca_file = src.tls_ca_file;
  cfg.leeway = src.leeway.count() < 0 ? std::chrono::milliseconds::zero() : src.leeway;
  cfg.refresh = src.refresh < std::chrono::seconds(1) ? std::chrono::seconds(1) : src.refresh;
  cfg.cache_entries = src.cache_entries;
  cfg.rs = cfg.es = cfg.ed = false;
  for (auto a : src.algorithms) {
    if (a == Alg::RS256) cfg.rs = true;
    else if (a == Alg::ES256) cfg.es = true;
    else if (a == Alg::EdDSA) cfg.ed = true;
  }
  if (!cfg.rs && !cfg.es && !cfg.ed) cfg.rs = cfg.es = cfg.ed = true;
  interval = cfg.refresh;
  std::call_once(g_metrics_once, [] { crocket::detail::add_metrics_renderer(&render_metrics); });

  if (!src.jwks_json.empty()) {
    auto parsed = detail::parse_jwks(src.jwks_json);
    if (!parsed) {
      log::warn("auth::Jwt inline JWKS was rejected: {reason}", parsed.error());
      return;
    }
    keys.store(std::move(*parsed));
    loaded.store(true, std::memory_order_release);
    return;
  }
  if (src.jwks.empty() && src.issuer.empty()) {
    log::warn("auth::Jwt has no issuer and no JWKS");
    return;
  }
  thread = std::thread([this] { loop(); });
}

Jwt::State::~State() {
  {
    std::lock_guard lk(mu);
    stop = true;
  }
  cv.notify_all();
  if (thread.joinable()) thread.join();
}

void Jwt::State::note_failure(std::string_view why) {
  g_refresh_failures.fetch_add(1, std::memory_order_relaxed);
  log::warn("jwks refresh failed: {reason}", std::string(why.substr(0, 300)));
}

void Jwt::State::nudge() {
  using namespace std::chrono;
  auto now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  auto prev = last_nudge.load(std::memory_order_relaxed);
  if (now - prev < duration_cast<nanoseconds>(seconds(30)).count()) return;
  if (!last_nudge.compare_exchange_strong(prev, now)) return;
  {
    std::lock_guard lk(mu);
    wake = true;
  }
  cv.notify_all();
}

bool Jwt::State::fetch() {
  std::string url = cfg.jwks;
  std::chrono::milliseconds cap = cfg.refresh;
  if (url.empty()) {
    std::string base = cfg.issuer;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
      note_failure("issuer is not configured");
      return false;
    }
    auto disc = detail::http_get(base + "/.well-known/openid-configuration", cfg.tls_ca_file);
    if (!disc) {
      note_failure(disc.error());
      return false;
    }
    if (disc->status != 200) {
      note_failure("openid-configuration HTTP " + std::to_string(disc->status));
      return false;
    }
    auto uri = detail::parse_discovery(disc->body, cfg.issuer);
    if (!uri) {
      note_failure(uri.error());
      return false;
    }
    url = std::move(*uri);
    if (disc->max_age.count() > 0)
      cap = std::min(cap, std::chrono::duration_cast<std::chrono::milliseconds>(disc->max_age));
  }
  auto body = detail::http_get(url, cfg.tls_ca_file);
  if (!body) {
    note_failure(body.error());
    return false;
  }
  if (body->status != 200) {
    note_failure("jwks HTTP " + std::to_string(body->status));
    return false;
  }
  auto parsed = detail::parse_jwks(body->body);
  if (!parsed) {
    note_failure(parsed.error());
    return false;
  }
  keys.store(std::move(*parsed), std::memory_order_release);
  loaded.store(true, std::memory_order_release);
  if (body->max_age.count() > 0)
    cap = std::min(cap, std::chrono::duration_cast<std::chrono::milliseconds>(body->max_age));
  if (cap < std::chrono::seconds(1)) cap = std::chrono::seconds(1);
  interval = cap;
  return true;
}

void Jwt::State::loop() {
  while (true) {
    bool ok = false;
    try {
      ok = fetch();
    } catch (const std::exception& e) {
      note_failure(e.what());
    } catch (...) {
      note_failure("unknown error");
    }
    std::unique_lock lk(mu);
    if (stop) return;
    auto wait = ok ? interval : std::min(interval, std::chrono::milliseconds(1000));
    cv.wait_for(lk, wait, [&] { return stop || wake; });
    wake = false;
    if (stop) return;
  }
}

bool Jwt::State::alg_ok(std::string_view alg) const {
  if (alg == "RS256") return cfg.rs;
  if (alg == "ES256") return cfg.es;
  if (alg == "EdDSA") return cfg.ed;
  return false;
}

std::expected<Authenticator::Verified, ApiError> Jwt::State::check_claims(std::string payload_json) {
  auto doc = json::parse(payload_json, token_json());
  if (!doc || !doc->is_object()) return invalid("payload is not a JSON object");

  if (auto* iss = doc->find("iss"); !iss || !iss->is_string() || iss->as_string() != cfg.issuer)
    return invalid("issuer does not match");
  if (!cfg.audience.empty()) {
    auto* aud = doc->find("aud");
    bool ok = false;
    if (aud && aud->is_string()) ok = aud->as_string() == cfg.audience;
    else if (aud && aud->is_array())
      for (auto& e : aud->as_array())
        if (e.is_string() && e.as_string() == cfg.audience) ok = true;
    if (!ok) return invalid("audience does not match");
  }

  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
  auto leeway = cfg.leeway.count();
  auto as_ms = [](std::int64_t seconds) -> std::optional<std::int64_t> {
    if (seconds < 0 || seconds > 10'000'000'000LL) return std::nullopt;
    return seconds * 1000;
  };
  std::optional<std::int64_t> exp_s;
  if (auto* exp = doc->find("exp")) {
    exp_s = json_seconds(exp);
    if (!exp_s) return invalid("exp is not a time");
    auto exp_ms = as_ms(*exp_s);
    if (!exp_ms) return invalid("exp is not a time");
    if (now_ms > *exp_ms + leeway) return coded("auth.expired", "token expired", "exp is past");
  }
  if (auto* nbf = doc->find("nbf")) {
    auto s = json_seconds(nbf);
    if (!s) return invalid("nbf is not a time");
    auto ms = as_ms(*s);
    if (!ms) return invalid("nbf is not a time");
    if (*ms > now_ms + leeway) return coded("auth.not_yet_valid", "token not yet valid", "nbf is in the future");
  }
  if (auto* iat = doc->find("iat")) {
    auto s = json_seconds(iat);
    if (!s) return invalid("iat is not a time");
    auto ms = as_ms(*s);
    if (!ms) return invalid("iat is not a time");
    if (*ms > now_ms + leeway) return coded("auth.not_yet_valid", "token not yet valid", "iat is in the future");
  }

  auto* sub = walk(*doc, cfg.subject_claim);
  if (!sub || !sub->is_string() || sub->as_string().empty()) return invalid("missing subject");

  Authenticator::Verified out;
  out.auth.subject = sub->as_string();
  strings_of(doc->find("scope"), out.auth.scopes);
  strings_of(walk(*doc, cfg.roles_claim), out.auth.roles);
  out.claims_json = std::move(payload_json);
  return out;
}

std::expected<Authenticator::Verified, ApiError> Jwt::State::verify(std::string_view token) {
  if (token.size() > 16 * 1024) return invalid("token is too long");
  if (token.find('\0') != std::string_view::npos || token.find('\r') != std::string_view::npos ||
      token.find('\n') != std::string_view::npos)
    return invalid("token is malformed");

  unsigned char sha[32]{};
  bool hashed = cfg.cache_entries > 0 && sha256(token, sha);
  if (hashed) {
    auto now_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto& e : g_cache.entries) {
      if (e.rep != cfg.id || std::memcmp(e.sha, sha, 32) != 0) continue;
      if (now_s >= e.exp) {
        e.rep = 0;
        break;
      }
      Authenticator::Verified hit;
      hit.auth.subject = e.subject;
      hit.auth.scopes = e.scopes;
      hit.auth.roles = e.roles;
      hit.claims_json = e.claims;
      return hit;
    }
  }

  auto dot1 = token.find('.');
  auto dot2 = dot1 == std::string_view::npos ? std::string_view::npos : token.find('.', dot1 + 1);
  if (dot1 == std::string_view::npos || dot2 == std::string_view::npos || token.find('.', dot2 + 1) != std::string_view::npos ||
      dot1 == 0 || dot2 == dot1 + 1)
    return invalid("token is not three base64url segments");
  auto signing = token.substr(0, dot2);
  auto h64 = token.substr(0, dot1);
  auto p64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
  auto s64 = token.substr(dot2 + 1);

  std::string header_json, payload_json, sig;
  if (!detail::b64url_decode(h64, header_json)) return invalid("header is not base64url");
  auto header = json::parse(header_json, token_json());
  if (!header || !header->is_object()) return invalid("header is not a JSON object");
  if (header->find("crit")) return invalid("unsupported crit header");

  auto* algv = header->find("alg");
  if (!algv || !algv->is_string()) return invalid("missing alg");
  auto alg = algv->as_string();
  if (alg == "none" || alg.starts_with("HS") || !alg_ok(alg)) return invalid("algorithm is not allowed");
  if (auto* typ = header->find("typ"); typ) {
    if (!typ->is_string()) return invalid("bad typ");
    std::string t = typ->as_string();
    for (auto& c : t)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (t != "jwt" && t != "at+jwt" && t != "application/at+jwt") return invalid("unsupported typ");
  }
  std::string kid;
  if (auto* k = header->find("kid")) {
    if (!k->is_string() || k->as_string().empty() || k->as_string().size() > 256) return invalid("bad kid");
    kid = k->as_string();
  }
  if (!detail::b64url_decode(s64, sig)) return invalid("signature is not base64url");

  if (!loaded.load(std::memory_order_acquire)) return invalid("signing keys are not loaded");
  auto set = keys.load(std::memory_order_acquire);
  if (!set) return invalid("signing keys are not loaded");

  std::vector<const detail::Key*> candidates;
  if (!kid.empty()) {
    bool saw = false;
    for (auto& k : set->keys) {
      if (k.kid != kid) continue;
      saw = true;
      if (detail::key_matches(k, alg)) candidates.push_back(&k);
    }
    if (!saw) {
      nudge();
      return coded("auth.unknown_key", "unknown signing key", "kid " + kid + " not in JWKS");
    }
    if (candidates.empty()) return invalid("signing key does not match alg");
  } else {
    for (auto& k : set->keys)
      if (detail::key_matches(k, alg)) candidates.push_back(&k);
    if (candidates.empty()) return invalid("no signing key for this algorithm");
  }
  bool signed_ok = false;
  for (auto* k : candidates)
    if (detail::verify_signature(*k, alg, signing, sig)) {
      signed_ok = true;
      break;
    }
  if (!signed_ok) return invalid("signature verification failed");

  if (!detail::b64url_decode(p64, payload_json)) return invalid("payload is not base64url");
  auto checked = check_claims(std::move(payload_json));
  if (!checked) return checked;

  if (hashed && cfg.cache_entries > 0) {
    auto doc = json::parse(checked->claims_json, token_json());
    auto exp = doc ? json_seconds(doc->find("exp")) : std::nullopt;
    if (exp && *exp > 0) {
      CacheEntry* slot = nullptr;
      if (g_cache.entries.size() < cfg.cache_entries) {
        g_cache.entries.emplace_back();
        slot = &g_cache.entries.back();
      } else {
        slot = &g_cache.entries[g_cache.next++ % g_cache.entries.size()];
      }
      slot->rep = cfg.id;
      std::memcpy(slot->sha, sha, 32);
      slot->exp = *exp;
      slot->claims = checked->claims_json;
      slot->subject = checked->auth.subject;
      slot->scopes = checked->auth.scopes;
      slot->roles = checked->auth.roles;
    }
  }
  return checked;
}

Jwt Jwt::from(const AuthConfig& cfg) {
  Jwt j;
  j.issuer = cfg.issuer;
  j.audience = cfg.audience;
  j.jwks = cfg.jwks;
  j.leeway = cfg.leeway;
  if (!cfg.roles_claim.empty()) j.roles_claim = cfg.roles_claim;
  if (cfg.jwks_refresh.count() > 0) j.refresh = cfg.jwks_refresh;
  return j;
}

Jwt Jwt::from_env() { return from(Config::from_env().auth); }

std::shared_ptr<Jwt::State> Jwt::activate() const {
  if (!state) state = std::make_shared<State>(*this);
  return state;
}

bool Jwt::ready() const { return state && state->ready(); }

Jwt::operator Authenticator() const {
  auto s = activate();
  return Authenticator([s](std::string_view token) -> std::expected<Authenticator::Verified, ApiError> {
    return s->verify(token);
  });
}

}  // namespace crocket::auth
