#include "request_clock.hpp"
#include "crocket/app.hpp"
#include "crocket/grpc.hpp"
#include "crocket/responder.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

namespace crocket {

void write_error(const ApiError& in, const Request& rq, Response& rs) {
  // A token or cookie that a verifier or a thrown exception copied into the
  // message must not reach the client, even in the dev profile's debug field.
  ApiError e = in;
  e.message = log::without_credentials(&rq, std::move(e.message));
  e.detail = log::without_credentials(&rq, std::move(e.detail));
  if (grpc::is_grpc_request(rq)) {
    grpc::detail::write_status(e, rq, rs);
    return;
  }
  rs.status = e.status;
  rs.error_code = e.code;
  rs.error_detail = e.detail;
  rs.set_content_type("application/problem+json");
  if (e.status == 401 && !rs.headers.contains("www-authenticate")) {
    // RFC 6750: a missing token is a challenge; a rejected one names invalid_token.
    // auth.cert_missing is a client certificate, not a bearer token.
    bool invalid = e.code.size() >= 5 && e.code.substr(0, 5) == "auth." && e.code != "auth.missing" &&
                   e.code != "auth.cert_missing";
    rs.headers.set("www-authenticate", invalid ? "Bearer error=\"invalid_token\"" : "Bearer");
  } else if (e.status == 403 && e.code == "auth.forbidden" && !rs.headers.contains("www-authenticate")) {
    rs.headers.set("www-authenticate", "Bearer error=\"insufficient_scope\"");
  }
  // RFC 9457. "detail" is the client message; ApiError::detail, which is for
  // logs, appears as "debug" in the dev profile only.
  auto& b = rs.body;
  b.clear();
  b += '{';
  if (!e.type.empty()) {
    b += R"("type":)";
    json::write_string(b, e.type);
    b += ',';
  }
  b += R"("title":)";
  json::write_string(b, detail::reason_phrase(e.status));
  b += R"(,"status":)";
  b += std::to_string(e.status);
  b += R"(,"detail":)";
  json::write_string(b, e.message);
  b += R"(,"code":)";
  json::write_string(b, e.code);
  b += R"(,"request_id":)";
  json::write_string(b, rq.request_id);
  if (!e.errors.empty()) {
    b += R"(,"errors":[)";
    for (std::size_t i = 0; i < e.errors.size(); ++i) {
      if (i) b += ',';
      b += R"({"pointer":)";
      json::write_string(b, e.errors[i].pointer);
      b += R"(,"detail":)";
      json::write_string(b, e.errors[i].detail);
      b += '}';
    }
    b += ']';
  }
  if (rq.dev_profile && !e.detail.empty()) {
    b += R"(,"debug":)";
    json::write_string(b, e.detail);
  }
  b += '}';
}

std::string IgniteError::message() const {
  std::string m = "ignite failed:";
  for (auto& p : problems) m += "\n  - " + p;
  return m;
}

namespace detail {

std::optional<std::string> normalize_path(std::string_view raw) {
  // Most paths have nothing to decode and no dot segments: as they are.
  if (!raw.empty() && raw.front() == '/' && raw.find('%') == std::string_view::npos) {
    bool dots = false;
    for (std::size_t at = raw.find("/."); at != std::string_view::npos; at = raw.find("/.", at + 1)) {
      auto rest = raw.substr(at + 1);
      auto seg = rest.substr(0, rest.find('/'));
      if (seg == "." || seg == "..") {
        dots = true;
        break;
      }
    }
    if (!dots) return std::string(raw);
  }
  std::vector<std::string> segs;
  if (!raw.empty() && raw.front() == '/') raw.remove_prefix(1);
  while (true) {
    auto slash = raw.find('/');
    bool last = slash == std::string_view::npos;
    std::string seg = percent_decode(raw.substr(0, slash), false);
    if (seg.find('\0') != std::string::npos) return std::nullopt;
    if (seg == "." || seg == "..") {
      if (seg == ".." && !segs.empty()) segs.pop_back();
      if (last) segs.emplace_back();  // "/a/." is "/a/"
    } else {
      std::string enc;
      enc.reserve(seg.size());
      for (char c : seg) {
        if (c == '%') enc += "%25";
        else if (c == '/') enc += "%2F";
        else enc += c;
      }
      segs.push_back(std::move(enc));
    }
    if (last) break;
    raw.remove_prefix(slash + 1);
  }
  std::string out = "/";
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i) out += '/';
    out += segs[i];
  }
  return out;
}

std::string percent_decode(std::string_view s, bool plus_is_space) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += char(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += (plus_is_space && s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view q) {
  std::vector<std::pair<std::string, std::string>> out;
  while (!q.empty()) {
    auto amp = q.find('&');
    auto kv = q.substr(0, amp);
    if (!kv.empty()) {
      auto eq = kv.find('=');
      out.emplace_back(percent_decode(kv.substr(0, eq), true),
                       eq == std::string_view::npos ? std::string() : percent_decode(kv.substr(eq + 1), true));
    }
    if (amp == std::string_view::npos) break;
    q.remove_prefix(amp + 1);
  }
  return out;
}

std::string generate_request_id() {
  thread_local std::mt19937_64 rng{std::random_device{}() ^
                                   std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
  static constexpr char hex[] = "0123456789abcdef";
  std::string id(32, '0');
  auto a = rng(), b = rng();
  for (int i = 0; i < 16; ++i) {
    id[i] = hex[(a >> (i * 4)) & 0xF];
    id[16 + i] = hex[(b >> (i * 4)) & 0xF];
  }
  return id;
}

std::string_view reason_phrase(int status) {
  // RFC 9110 section 15, plus 428, 429, 431 (RFC 6585) and 451 (RFC 7725).
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
  }
  // Any other code: its class, so a problem's title is never empty.
  if (status >= 100 && status < 200) return "Informational";
  if (status >= 200 && status < 300) return "Success";
  if (status >= 300 && status < 400) return "Redirection";
  if (status >= 400 && status < 500) return "Client Error";
  if (status >= 500 && status < 600) return "Server Error";
  return "Unknown Status";
}

std::chrono::system_clock::time_point wall_time(Clock::time_point at) {
  using namespace std::chrono;
  thread_local Clock::time_point base_steady{};
  thread_local system_clock::time_point base_wall{};
  if (base_steady == Clock::time_point{} || at - base_steady >= seconds(1) || at < base_steady) {
    base_steady = Clock::now();
    base_wall = system_clock::now();
  }
  return base_wall + duration_cast<system_clock::duration>(at - base_steady);
}

void append_iso8601_now(std::string& out) { append_iso8601(out, std::chrono::system_clock::now()); }

void append_iso8601(std::string& out, std::chrono::system_clock::time_point now) {
  using namespace std::chrono;
  auto secs = time_point_cast<seconds>(now);
  auto ms = duration_cast<milliseconds>(now - secs).count();
  // The date and time change once a second: format them then, per thread.
  thread_local std::int64_t cached_secs = -1;
  thread_local char cached[24];  // "2026-10-07T16:53:20."
  if (secs.time_since_epoch().count() != cached_secs) {
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];  // room for any int the compiler assumes tm fields may hold
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::memcpy(cached, buf, 20);
    cached_secs = secs.time_since_epoch().count();
  }
  out.append(cached, 20);
  out += char('0' + ms / 100);
  out += char('0' + ms / 10 % 10);
  out += char('0' + ms % 10);
  out += 'Z';
}

std::string iso8601_now() {
  std::string s;
  append_iso8601_now(s);
  return s;
}

}  // namespace detail
}  // namespace crocket
