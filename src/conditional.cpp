#include "conditional.hpp"

#include "crocket/responder.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <format>

namespace crocket {
namespace http {

namespace {
constexpr std::string_view kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}  // namespace

std::string date(std::chrono::system_clock::time_point t) {
  using namespace std::chrono;
  auto secs = floor<seconds>(t);
  auto day = floor<days>(secs);
  year_month_day ymd{day};
  hh_mm_ss hms{secs - day};
  return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT", kDays[weekday(day).c_encoding()],
                     unsigned(ymd.day()), kMonths[unsigned(ymd.month()) - 1], int(ymd.year()), hms.hours().count(),
                     hms.minutes().count(), hms.seconds().count());
}

std::optional<std::chrono::sys_seconds> parse_date(std::string_view s) {
  using namespace std::chrono;
  // "Sun, 06 Nov 1994 08:49:37 GMT"
  if (s.size() != 29 || s.substr(3, 2) != ", " || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' ||
      s[22] != ':' || s.substr(25) != " GMT")
    return std::nullopt;
  auto num = [&](std::size_t at, std::size_t n, int& out) {
    auto r = std::from_chars(s.data() + at, s.data() + at + n, out);
    return r.ec == std::errc() && r.ptr == s.data() + at + n;
  };
  int d, y, hh, mm, ss;
  if (!num(5, 2, d) || !num(12, 4, y) || !num(17, 2, hh) || !num(20, 2, mm) || !num(23, 2, ss)) return std::nullopt;
  unsigned m = 0;
  for (unsigned i = 0; i < 12; ++i)
    if (s.substr(8, 3) == kMonths[i]) m = i + 1;
  year_month_day ymd{year(y), month(m), day(unsigned(d))};
  if (!m || !ymd.ok() || hh > 23 || mm > 59 || ss > 60) return std::nullopt;
  return sys_days(ymd) + hours(hh) + minutes(mm) + seconds(std::min(ss, 59));
}

}  // namespace http

namespace detail {
namespace {

/// A 64-bit hash of the body: fast, and stable across processes and machines,
/// so every instance behind a load balancer gives a response the same ETag.
std::uint64_t body_hash(std::string_view s) {
  constexpr std::uint64_t k = 0x9E3779B97F4A7C15ULL;
  std::uint64_t h = s.size() * k;
  std::size_t i = 0;
  for (; i + 8 <= s.size(); i += 8) {
    std::uint64_t w;
    std::memcpy(&w, s.data() + i, 8);
    h = (h ^ w) * k;
    h ^= h >> 29;
  }
  std::uint64_t tail = 0;
  std::memcpy(&tail, s.data() + i, s.size() - i);
  h = (h ^ tail) * k;
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  return h ^ (h >> 32);
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

/// The quoted part of an entity tag, without W/: `W/"x"` -> `"x"`.
std::string_view opaque(std::string_view tag) { return tag.starts_with("W/") ? tag.substr(2) : tag; }

/// If-None-Match: "*" or a list of entity tags, compared weakly (RFC 9110 13.1.2).
bool none_match(std::string_view header, std::string_view etag) {
  header = trim(header);
  if (header == "*") return true;
  while (!header.empty()) {
    header = trim(header);
    if (header.starts_with(',')) {
      header.remove_prefix(1);
      continue;
    }
    std::size_t start = header.starts_with("W/") ? 2 : 0;
    if (header.size() <= start || header[start] != '"') return false;  // malformed: no match
    auto close = header.find('"', start + 1);
    if (close == std::string_view::npos) return false;
    if (header.substr(start, close - start + 1) == opaque(etag)) return true;
    header.remove_prefix(close + 1);
  }
  return false;
}

void not_modified(Response& rs) {
  rs.status = 304;
  rs.body.clear();
}

/// A single range "bytes=a-b", "bytes=a-" or "bytes=-n" against `size`, as
/// [first, last]. Several ranges or bad syntax: nullopt, and the whole body is
/// sent (a server may ignore Range). Unsatisfiable: first > last.
std::optional<std::pair<std::size_t, std::size_t>> parse_range(std::string_view h, std::size_t size) {
  h = trim(h);
  if (!h.starts_with("bytes=")) return std::nullopt;
  h = trim(h.substr(6));
  if (h.find(',') != std::string_view::npos) return std::nullopt;
  auto dash = h.find('-');
  if (dash == std::string_view::npos) return std::nullopt;
  auto number = [](std::string_view d, std::size_t& out) {
    d = trim(d);
    auto r = std::from_chars(d.data(), d.data() + d.size(), out);
    return !d.empty() && r.ec == std::errc() && r.ptr == d.data() + d.size();
  };
  std::size_t a, b;
  if (trim(h.substr(0, dash)).empty()) {  // suffix: the last n bytes
    if (!number(h.substr(dash + 1), b)) return std::nullopt;
    if (b == 0 || size == 0) return std::pair<std::size_t, std::size_t>{1, 0};
    return std::pair{size - std::min(b, size), size - 1};
  }
  if (!number(h.substr(0, dash), a)) return std::nullopt;
  if (trim(h.substr(dash + 1)).empty()) b = size - 1;
  else if (!number(h.substr(dash + 1), b) || b < a) return std::nullopt;
  if (a >= size) return std::pair<std::size_t, std::size_t>{1, 0};
  return std::pair{a, std::min(b, size - 1)};
}

}  // namespace

void conditional(const Request& rq, Response& rs) {
  if (rs.status != 200 || (rq.method != http::Method::Get && rq.method != http::Method::Head)) return;
  if (!rs.headers.contains("etag")) {
    // "<16 hex digits>", written directly: this runs for every GET.
    static constexpr char hex[] = "0123456789abcdef";
    char tag[18] = {'"'};
    std::uint64_t h = body_hash(rs.body);
    for (int i = 16; i >= 1; --i, h >>= 4) tag[i] = hex[h & 0xF];
    tag[17] = '"';
    rs.headers.add("etag", std::string_view(tag, sizeof tag));
  }
  std::string etag(*rs.headers.get("etag"));
  auto last_modified = rs.headers.get("last-modified");

  // If-None-Match decides when present; If-Modified-Since only without it.
  if (auto inm = rq.header("if-none-match")) {
    if (none_match(*inm, etag)) return not_modified(rs);
  } else if (auto ims = rq.header("if-modified-since"); ims && last_modified) {
    auto since = http::parse_date(*ims);
    auto modified = http::parse_date(*last_modified);
    if (since && modified && *modified <= *since) return not_modified(rs);
  }

  auto range = rq.header("range");
  auto accepts = rs.headers.get("accept-ranges");
  if (!range || rq.method != http::Method::Get || !accepts || trim(*accepts) != "bytes") return;
  // If-Range: the range is for the representation the client already has.
  if (auto if_range = rq.header("if-range")) {
    auto v = trim(*if_range);
    bool same = v.starts_with('"') ? (!etag.starts_with("W/") && v == etag)
                                   : (last_modified && http::parse_date(v) && v == trim(*last_modified));
    if (!same) return;
  }
  auto r = parse_range(*range, rs.body.size());
  if (!r) return;
  std::size_t size = rs.body.size();
  if (r->first > r->second) {
    write_error({416, "range.unsatisfiable", "range not satisfiable", std::string(*range)}, rq, rs);
    rs.headers.set("content-range", std::format("bytes */{}", size));
    return;
  }
  rs.status = 206;
  rs.headers.set("content-range", std::format("bytes {}-{}/{}", r->first, r->second, size));
  rs.body = rs.body.substr(r->first, r->second - r->first + 1);
}

}  // namespace detail
}  // namespace crocket
