#include "internal.hpp"

#include "crocket/json.hpp"

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace crocket::auth::detail {
namespace {

// TODO: stopgap. Replace this hand-rolled HTTPS GET with crocket::client when
// that lands (Roadmap #37 / RESILIENCE). That client is built on h2o_httpclient.
// Do not call h2o directly from crocket_auth.

constexpr int kTimeoutMs = 5000;
constexpr std::size_t kMaxBody = 1 << 20;

struct Url {
  bool https = false;
  std::string host;
  std::string port;
  std::string path;
};

std::expected<Url, std::string> parse_url(std::string_view url) {
  Url u;
  if (url.starts_with("https://")) {
    u.https = true;
    u.port = "443";
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    u.port = "80";
    url.remove_prefix(7);
  } else {
    return std::unexpected("URL scheme must be https or http");
  }
  auto slash = url.find('/');
  auto hostport = url.substr(0, slash);
  u.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  if (hostport.empty()) return std::unexpected("URL has no host");
  if (hostport.front() == '[') {
    auto end = hostport.find(']');
    if (end == std::string_view::npos) return std::unexpected("bad IPv6 host");
    u.host = std::string(hostport.substr(1, end - 1));
    if (end + 1 < hostport.size()) {
      if (hostport[end + 1] != ':') return std::unexpected("bad host");
      u.port = std::string(hostport.substr(end + 2));
    }
  } else if (auto colon = hostport.rfind(':'); colon != std::string_view::npos && hostport.find(':') == colon) {
    u.host = std::string(hostport.substr(0, colon));
    u.port = std::string(hostport.substr(colon + 1));
  } else {
    u.host = std::string(hostport);
  }
  if (u.host.empty() || u.port.empty()) return std::unexpected("URL has no host");
  return u;
}

std::string origin_of(const Url& u) {
  std::string o = u.https ? "https://" : "http://";
  if (u.host.find(':') != std::string::npos) o += "[" + u.host + "]";
  else o += u.host;
  bool standard = (u.https && u.port == "443") || (!u.https && u.port == "80");
  if (!standard) o += ":" + u.port;
  return o;
}

std::string resolve(const Url& base, std::string_view loc) {
  if (loc.starts_with("https://") || loc.starts_with("http://")) return std::string(loc);
  if (loc.starts_with("/")) return origin_of(base) + std::string(loc);
  return {};
}

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  Fd() = default;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
};

bool wait_fd(int fd, short ev) {
  pollfd p{fd, ev, 0};
  for (;;) {
    int rc = ::poll(&p, 1, kTimeoutMs);
    if (rc > 0) return true;
    if (rc == 0) return false;
    if (errno != EINTR) return false;
  }
}

std::expected<Fd, std::string> connect_to(const Url& u) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0)
    return std::unexpected("DNS lookup failed for " + u.host);
  std::string last = "connection failed";
  for (auto* ai = res; ai; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
      last = "connection refused";
      ::close(fd);
      continue;
    }
    if (rc < 0 && !wait_fd(fd, POLLOUT)) {
      last = "connection timed out";
      ::close(fd);
      continue;
    }
    int so = 0;
    socklen_t sl = sizeof so;
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
    if (so != 0) {
      last = so == ECONNREFUSED ? "connection refused" : "connection failed";
      ::close(fd);
      continue;
    }
    ::fcntl(fd, F_SETFL, flags);
    timeval tv{kTimeoutMs / 1000, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    freeaddrinfo(res);
    Fd out;
    out.fd = fd;
    return out;
  }
  freeaddrinfo(res);
  return std::unexpected(last);
}

std::expected<std::size_t, std::string> read_some(int fd, char* buf, std::size_t n) {
  for (;;) {
    ssize_t got = ::recv(fd, buf, n, 0);
    if (got > 0) return static_cast<std::size_t>(got);
    if (got == 0) return 0;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return std::unexpected("read timed out");
    return std::unexpected("read failed");
  }
}

struct Tls {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  ~Tls() {
    if (ssl) SSL_free(ssl);
    if (ctx) SSL_CTX_free(ctx);
  }
};

std::expected<std::unique_ptr<Tls>, std::string> tls_wrap(int fd, const Url& u, std::string_view ca_file) {
  auto tls = std::make_unique<Tls>();
  tls->ctx = SSL_CTX_new(TLS_client_method());
  if (!tls->ctx) return std::unexpected("TLS setup failed");
  SSL_CTX_set_min_proto_version(tls->ctx, TLS1_2_VERSION);
  SSL_CTX_set_verify(tls->ctx, SSL_VERIFY_PEER, nullptr);
  if (!ca_file.empty()) {
    if (SSL_CTX_load_verify_locations(tls->ctx, std::string(ca_file).c_str(), nullptr) != 1)
      return std::unexpected("could not load CA file");
  } else if (SSL_CTX_set_default_verify_paths(tls->ctx) != 1) {
    return std::unexpected("could not load the default CA store");
  }
  tls->ssl = SSL_new(tls->ctx);
  if (!tls->ssl) return std::unexpected("TLS setup failed");
  SSL_set_fd(tls->ssl, fd);
  SSL_set_tlsext_host_name(tls->ssl, u.host.c_str());
  SSL_set1_host(tls->ssl, u.host.c_str());
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  for (;;) {
    int rc = SSL_connect(tls->ssl);
    if (rc == 1) break;
    int err = SSL_get_error(tls->ssl, rc);
    short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
    if (!ev || !wait_fd(fd, ev)) {
      ::fcntl(fd, F_SETFL, flags);
      return std::unexpected("TLS handshake failed");
    }
  }
  ::fcntl(fd, F_SETFL, flags);
  return tls;
}

std::expected<std::size_t, std::string> tls_read(SSL* ssl, int fd, char* buf, std::size_t n) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  for (;;) {
    int got = SSL_read(ssl, buf, static_cast<int>(n));
    if (got > 0) {
      ::fcntl(fd, F_SETFL, flags);
      return static_cast<std::size_t>(got);
    }
    int err = SSL_get_error(ssl, got);
    if (err == SSL_ERROR_ZERO_RETURN) {
      ::fcntl(fd, F_SETFL, flags);
      return 0;
    }
    short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
    if (!ev || !wait_fd(fd, ev)) {
      ::fcntl(fd, F_SETFL, flags);
      return std::unexpected(err == SSL_ERROR_SYSCALL || err == SSL_ERROR_SSL ? "TLS read failed" : "read timed out");
    }
  }
}

std::expected<void, std::string> write_all(int fd, SSL* ssl, std::string_view data) {
  while (!data.empty()) {
    int n = 0;
    if (ssl) {
      int flags = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      n = SSL_write(ssl, data.data(), static_cast<int>(data.size()));
      if (n <= 0) {
        int err = SSL_get_error(ssl, n);
        short ev = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
        if (!ev || !wait_fd(fd, ev)) {
          ::fcntl(fd, F_SETFL, flags);
          return std::unexpected("write failed");
        }
        ::fcntl(fd, F_SETFL, flags);
        continue;
      }
      ::fcntl(fd, F_SETFL, flags);
    } else {
      ssize_t got = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
      if (got < 0) {
        if (errno == EINTR) continue;
        return std::unexpected(errno == EAGAIN ? "write timed out" : "write failed");
      }
      n = static_cast<int>(got);
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return {};
}

std::chrono::seconds max_age_of(std::string_view cache_control) {
  auto at = cache_control.find("max-age");
  if (at == std::string_view::npos) return {};
  auto eq = cache_control.find('=', at);
  if (eq == std::string_view::npos) return {};
  auto num = cache_control.substr(eq + 1);
  while (!num.empty() && num.front() == ' ') num.remove_prefix(1);
  std::uint64_t v = 0;
  auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
  if (ec != std::errc() || v == 0 || v > 86400ull * 30) return {};
  return std::chrono::seconds(v);
}

std::expected<std::string, std::string> decode_chunks(std::string_view in) {
  std::string out;
  while (!in.empty()) {
    auto line = in.find("\r\n");
    if (line == std::string_view::npos) return std::unexpected("truncated chunk");
    auto hex = in.substr(0, line);
    if (auto semi = hex.find(';'); semi != std::string_view::npos) hex = hex.substr(0, semi);
    std::size_t n = 0;
    auto [p, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), n, 16);
    if (ec != std::errc() || p != hex.data() + hex.size()) return std::unexpected("bad chunk size");
    in.remove_prefix(line + 2);
    if (n == 0) return out;
    if (in.size() < n + 2 || out.size() + n > kMaxBody) return std::unexpected("truncated chunk");
    out.append(in.substr(0, n));
    in.remove_prefix(n);
    if (!in.starts_with("\r\n")) return std::unexpected("bad chunk");
    in.remove_prefix(2);
  }
  return std::unexpected("truncated chunk");
}

using Reader = std::expected<std::size_t, std::string> (*)(void*, char*, std::size_t);

struct ReaderCtx {
  int fd;
  SSL* ssl;
};

std::expected<std::size_t, std::string> read_ctx(void* p, char* buf, std::size_t n) {
  auto* c = static_cast<ReaderCtx*>(p);
  if (c->ssl) return tls_read(c->ssl, c->fd, buf, n);
  return read_some(c->fd, buf, n);
}

std::expected<Fetched, std::string> read_response(ReaderCtx ctx) {
  std::string raw;
  char buf[8192];
  auto pull = [&]() -> std::expected<void, std::string> {
    if (raw.size() >= kMaxBody + 8192) return std::unexpected("response is too large");
    auto n = read_ctx(&ctx, buf, sizeof buf);
    if (!n) return std::unexpected(n.error());
    if (*n == 0) return {};
    raw.append(buf, *n);
    return {};
  };
  for (;;) {
    if (raw.find("\r\n\r\n") != std::string::npos) break;
    auto before = raw.size();
    if (auto ok = pull(); !ok) return std::unexpected(ok.error());
    if (raw.size() == before) return std::unexpected("response ended before headers");
  }
  auto hdr_end = raw.find("\r\n\r\n");
  auto head = std::string_view(raw).substr(0, hdr_end);
  auto line_end = head.find("\r\n");
  auto status_line = head.substr(0, line_end);
  auto sp1 = status_line.find(' ');
  if (sp1 == std::string_view::npos) return std::unexpected("bad status line");
  int status = 0;
  auto code = status_line.substr(sp1 + 1);
  auto sp2 = code.find(' ');
  if (sp2 != std::string_view::npos) code = code.substr(0, sp2);
  auto [p, ec] = std::from_chars(code.data(), code.data() + code.size(), status);
  if (ec != std::errc() || p != code.data() + code.size()) return std::unexpected("bad status line");

  std::optional<std::size_t> content_length;
  bool chunked = false;
  std::chrono::seconds max_age{};
  std::string location;
  auto rest = head;
  if (line_end != std::string_view::npos) rest.remove_prefix(line_end + 2);
  while (!rest.empty()) {
    auto nl = rest.find("\r\n");
    auto line = rest.substr(0, nl);
    auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      auto name = line.substr(0, colon);
      auto value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      std::string lower(name);
      for (auto& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (lower == "content-length") {
        std::size_t n = 0;
        auto [q, e2] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (e2 == std::errc() && q == value.data() + value.size() && n <= kMaxBody) content_length = n;
      } else if (lower == "transfer-encoding" && value.find("chunked") != std::string_view::npos) {
        chunked = true;
      } else if (lower == "cache-control") {
        max_age = max_age_of(value);
      } else if (lower == "location") {
        location = std::string(value);
      }
    }
    if (nl == std::string_view::npos) break;
    rest.remove_prefix(nl + 2);
  }

  std::string body = raw.substr(hdr_end + 4);
  auto need = [&](std::size_t n) -> std::expected<void, std::string> {
    while (body.size() < n) {
      auto before = raw.size();
      if (auto ok = pull(); !ok) return std::unexpected(ok.error());
      if (raw.size() == before) return std::unexpected("response body ended early");
      body = raw.substr(hdr_end + 4);
    }
    return {};
  };
  if (content_length && !chunked) {
    if (auto ok = need(*content_length); !ok) return std::unexpected(ok.error());
    body.resize(*content_length);
  } else if (chunked) {
    for (;;) {
      body = raw.substr(hdr_end + 4);
      auto decoded = decode_chunks(body);
      if (decoded) {
        body = std::move(*decoded);
        break;
      }
      auto before = raw.size();
      if (auto ok = pull(); !ok) return std::unexpected(ok.error());
      if (raw.size() == before) return std::unexpected(decoded.error());
    }
  } else {
    for (;;) {
      auto before = raw.size();
      auto more = pull();
      if (!more) {
        if (more.error() == "read timed out" && !body.empty()) break;
        return std::unexpected(more.error());
      }
      if (raw.size() == before) break;
      body = raw.substr(hdr_end + 4);
      if (body.size() > kMaxBody) return std::unexpected("response is too large");
    }
  }
  Fetched out;
  out.status = status;
  out.max_age = max_age;
  // 3xx: the body carries the Location target for the redirect loop.
  out.body = status >= 300 && status < 400 ? std::move(location) : std::move(body);
  return out;
}

std::expected<Fetched, std::string> http_get_once(const Url& u, std::string_view ca_file) {
  auto fd = connect_to(u);
  if (!fd) return std::unexpected(fd.error());
  std::unique_ptr<Tls> tls;
  SSL* ssl = nullptr;
  if (u.https) {
    auto wrapped = tls_wrap(fd->fd, u, ca_file);
    if (!wrapped) return std::unexpected(wrapped.error());
    tls = std::move(*wrapped);
    ssl = tls->ssl;
  }
  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
  bool standard = (u.https && u.port == "443") || (!u.https && u.port == "80");
  if (!standard) req += ":" + u.port;
  req += "\r\nUser-Agent: crocket-auth\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
  if (auto ok = write_all(fd->fd, ssl, req); !ok) return std::unexpected(ok.error());
  return read_response(ReaderCtx{fd->fd, ssl});
}

}  // namespace

std::expected<Fetched, std::string> http_get(std::string_view url, std::string_view ca_file) {
  std::string current(url);
  for (int hop = 0; hop < 4; ++hop) {
    auto parsed = parse_url(current);
    if (!parsed) return std::unexpected(parsed.error());
    auto res = http_get_once(*parsed, ca_file);
    if (!res) return res;
    if (res->status >= 300 && res->status < 400 && hop < 3) {
      auto next = resolve(*parsed, res->body);
      if (next.empty()) return std::unexpected("redirect with no Location");
      current = std::move(next);
      continue;
    }
    return res;
  }
  return std::unexpected("too many redirects");
}

std::expected<std::string, std::string> parse_discovery(std::string_view json, std::string_view expected_issuer) {
  auto doc = json::parse(json);
  if (!doc || !doc->is_object()) return std::unexpected("openid-configuration is not a JSON object");
  auto* iss = doc->find("issuer");
  auto* jwks = doc->find("jwks_uri");
  if (!iss || !iss->is_string()) return std::unexpected("openid-configuration has no issuer");
  if (iss->as_string() != expected_issuer)
    return std::unexpected("openid-configuration issuer does not match");
  if (!jwks || !jwks->is_string() || jwks->as_string().empty())
    return std::unexpected("openid-configuration has no jwks_uri");
  auto uri = jwks->as_string();
  if (!uri.starts_with("https://") && !uri.starts_with("http://"))
    return std::unexpected("jwks_uri is not an http(s) URL");
  return uri;
}

}  // namespace crocket::auth::detail
