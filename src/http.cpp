#include "crocket/app.hpp"
#include "crocket/grpc.hpp"
#include "crocket/responder.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace crocket {

void write_error(const ApiError& e, const Request& rq, Response& rs) {
  if (grpc::is_grpc_request(rq)) {
    grpc::detail::write_status(e, rq, rs);
    return;
  }
  rs.status = e.status;
  rs.error_code = e.code;
  rs.error_detail = e.detail;
  rs.set_content_type("application/problem+json");
  if (e.status == 401 && !rs.headers.contains("www-authenticate"))
    rs.headers.set("www-authenticate", "Bearer");
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

// ---- Accept ------------------------------------------------------------------------

namespace {
std::string_view trim_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}
}  // namespace

Accept::Accept(std::string_view header) {
  while (!header.empty()) {
    auto comma = header.find(',');
    auto item = trim_ows(header.substr(0, comma));
    header.remove_prefix(comma == std::string_view::npos ? header.size() : comma + 1);
    auto media = trim_ows(item.substr(0, item.find(';')));
    auto slash = media.find('/');
    if (slash == std::string_view::npos) continue;
    double q = 1;
    if (auto semi = item.find(';'); semi != std::string_view::npos) {
      auto params = item.substr(semi + 1);
      while (true) {
        auto next = params.find(';');
        auto p = trim_ows(params.substr(0, next));
        if (p.size() > 2 && (p[0] == 'q' || p[0] == 'Q') && p[1] == '=')
          if (std::from_chars(p.data() + 2, p.data() + p.size(), q).ec != std::errc() || q < 0 || q > 1) q = 0;
        if (next == std::string_view::npos) break;
        params.remove_prefix(next + 1);
      }
    }
    ranges_.push_back({lower_ascii(media.substr(0, slash)), lower_ascii(media.substr(slash + 1)), q});
  }
}

double Accept::quality(std::string_view type) const {
  if (ranges_.empty()) return 1;
  auto t = lower_ascii(trim_ows(type.substr(0, type.find(';'))));
  auto slash = t.find('/');
  std::string_view main = std::string_view(t).substr(0, slash);
  std::string_view sub = slash == std::string::npos ? std::string_view() : std::string_view(t).substr(slash + 1);
  // The most specific matching range decides: text/csv over text/* over */*.
  int best = -1;
  double q = 0;
  for (auto& r : ranges_) {
    int specificity = r.type == "*" ? (r.subtype == "*" ? 0 : -1)
                      : r.type != main ? -1
                      : r.subtype == "*" ? 1
                      : r.subtype == sub ? 2
                                         : -1;
    if (specificity > best) best = specificity, q = r.q;
  }
  return best < 0 ? 0 : q;
}

std::string_view Accept::best(std::initializer_list<std::string_view> offered) const {
  std::string_view choice = offered.size() ? *offered.begin() : std::string_view();
  double top = 0;
  for (auto o : offered)
    if (double q = quality(o); q > top) top = q, choice = o;
  return choice;
}

bool Accept::accepts(std::string_view type) const { return quality(type) > 0; }

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
  char buf[64];  // room for any int the compiler assumes tm fields may hold
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
  return buf;
}

}  // namespace detail
}  // namespace crocket
