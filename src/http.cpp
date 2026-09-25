#include "crocket/app.hpp"
#include "crocket/responder.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace crocket {

void write_error(const ApiError& e, const Request& rq, Response& rs) {
  rs.status = e.status;
  rs.error_code = e.code;
  rs.error_detail = e.detail;
  rs.set_content_type("application/json");
  if (e.status == 401 && !rs.headers.contains("www-authenticate"))
    rs.headers.set("www-authenticate", "Bearer");
  rs.body.clear();
  rs.body += R"({"error":{"code":)";
  json::write_string(rs.body, e.code);
  rs.body += R"(,"message":)";
  json::write_string(rs.body, e.message);
  rs.body += R"(,"request_id":)";
  json::write_string(rs.body, rq.request_id);
  rs.body += "}}";
}

std::string IgniteError::message() const {
  std::string m = "ignite failed:";
  for (auto& p : problems) m += "\n  - " + p;
  return m;
}

namespace detail {

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
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
  }
}

std::string iso8601_now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto secs = time_point_cast<seconds>(now);
  auto ms = duration_cast<milliseconds>(now - secs).count();
  std::time_t t = system_clock::to_time_t(secs);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  return buf;
}

}  // namespace detail
}  // namespace crocket
