#include "crocket/json.hpp"

#include <cctype>
#include <cstring>

namespace crocket::json {
namespace {

/// Length of the well-formed UTF-8 sequence at p (Unicode 15, table 3-7), or 0.
/// Rejects overlong forms, surrogates and code points above U+10FFFF.
std::size_t utf8_sequence(const unsigned char* p, const unsigned char* end) {
  unsigned char b = p[0];
  if (b < 0x80) return 1;
  auto cont = [&](std::size_t i, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
    return p + i < end && p[i] >= lo && p[i] <= hi;
  };
  if (b >= 0xC2 && b <= 0xDF) return cont(1) ? 2 : 0;
  if (b == 0xE0) return cont(1, 0xA0) && cont(2) ? 3 : 0;
  if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) return cont(1) && cont(2) ? 3 : 0;
  if (b == 0xED) return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;
  if (b == 0xF0) return cont(1, 0x90) && cont(2) && cont(3) ? 4 : 0;
  if (b >= 0xF1 && b <= 0xF3) return cont(1) && cont(2) && cont(3) ? 4 : 0;
  if (b == 0xF4) return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0;
  return 0;
}

void put_utf8(std::string& o, std::uint32_t cp) {
  if (cp < 0x80) o += char(cp);
  else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
  else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}

/// The code point at s[i] (U+FFFD for an invalid byte) and its length.
std::pair<char32_t, std::size_t> decode_utf8(std::string_view s, std::size_t i) {
  auto p = reinterpret_cast<const unsigned char*>(s.data()) + i;
  auto end = reinterpret_cast<const unsigned char*>(s.data()) + s.size();
  std::size_t n = utf8_sequence(p, end);
  switch (n) {
    case 1: return {p[0], 1};
    case 2: return {char32_t(p[0] & 0x1F) << 6 | (p[1] & 0x3F), 2};
    case 3: return {char32_t(p[0] & 0x0F) << 12 | char32_t(p[1] & 0x3F) << 6 | (p[2] & 0x3F), 3};
    case 4: return {char32_t(p[0] & 0x07) << 18 | char32_t(p[1] & 0x3F) << 12 | char32_t(p[2] & 0x3F) << 6 | (p[3] & 0x3F), 4};
    default: return {0xFFFD, 1};
  }
}

bool digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// ---- errors ------------------------------------------------------------------------

std::string Error::describe() const {
  switch (code) {
    case Errc::syntax:
      return "malformed JSON at line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
    case Errc::validation: {
      std::string out;
      for (auto& e : errors) {
        if (!out.empty()) out += "; ";
        out += (e.pointer.empty() ? std::string("body") : "field '" + e.pointer + "'") + ": " + e.detail;
      }
      return out;
    }
    default:
      return (pointer.empty() ? std::string("body") : "field '" + pointer + "'") + ": " + message;
  }
}

namespace detail {

std::string pointer(const Path* p) {
  std::vector<const Path*> chain;
  for (; p; p = p->parent) chain.push_back(p);
  std::string out;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    out += '/';
    if ((*it)->is_index) {
      out += std::to_string((*it)->index);
      continue;
    }
    for (char c : (*it)->key) {
      if (c == '~') out += "~0";
      else if (c == '/') out += "~1";
      else out += c;
    }
  }
  return out;
}

void bound_message(std::string& out, std::string_view word, bool integral, long long i, double d) {
  char buf[64];
  auto r = integral ? std::to_chars(buf, buf + sizeof buf, i) : std::to_chars(buf, buf + sizeof buf, d);
  out = "must be ";
  out += word;
  out += ' ';
  out.append(buf, r.ptr);
}

std::size_t code_points(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return n;
}

bool valid_email(std::string_view s) {
  if (s.size() > 254) return false;
  auto at = s.rfind('@');
  if (at == std::string_view::npos || at == 0 || at > 64) return false;
  auto local = s.substr(0, at), domain = s.substr(at + 1);
  // Local part: dot-atom (RFC 5322) plus UTF-8 (RFC 6531).
  if (local.front() == '.' || local.back() == '.') return false;
  for (std::size_t i = 0; i < local.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(local[i]);
    if (c == '.' && local[i + 1] == '.') return false;
    if (c >= 0x80 || std::isalnum(c) || (c != 0 && std::strchr(".!#$%&'*+/=?^_`{|}~-", c))) continue;
    return false;
  }
  // Domain: two or more labels of letters, digits and hyphens (or UTF-8), and
  // a top-level label that is not all digits.
  if (domain.empty() || domain.size() > 253) return false;
  std::size_t labels = 0;
  std::string_view last;
  while (true) {
    auto dot = domain.find('.');
    auto label = domain.substr(0, dot);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
    for (char ch : label) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (!(c >= 0x80 || std::isalnum(c) || c == '-')) return false;
    }
    ++labels;
    last = label;
    if (dot == std::string_view::npos) break;
    domain = domain.substr(dot + 1);
  }
  return labels >= 2 && last.find_first_not_of("0123456789") != std::string_view::npos;
}

// ---- dates and times ---------------------------------------------------------------

namespace {

void put_digits(std::string& out, std::int64_t v, int width) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v < 0 ? -v : v);
  if (v < 0) out += '-';
  for (int pad = width - int(r.ptr - buf); pad > 0; --pad) out += '0';
  out.append(buf, r.ptr);
}

bool fixed_digits(std::string_view s, std::size_t at, int n, int& out) {
  if (at + n > s.size()) return false;
  out = 0;
  for (int i = 0; i < n; ++i) {
    char c = s[at + i];
    if (!digit(c)) return false;
    out = out * 10 + (c - '0');
  }
  return true;
}

bool date_part(std::string_view s, std::int64_t& days) {
  int y, m, d;
  if (s.size() < 10 || !fixed_digits(s, 0, 4, y) || s[4] != '-' || !fixed_digits(s, 5, 2, m) || s[7] != '-' ||
      !fixed_digits(s, 8, 2, d))
    return false;
  std::chrono::year_month_day ymd{std::chrono::year(y), std::chrono::month(unsigned(m)), std::chrono::day(unsigned(d))};
  if (!ymd.ok()) return false;
  days = std::chrono::sys_days(ymd).time_since_epoch().count();
  return true;
}

}  // namespace

void write_date(std::string& out, std::int64_t days) {
  std::chrono::year_month_day ymd{std::chrono::sys_days(std::chrono::days(days))};
  put_digits(out, int(ymd.year()), 4);
  out += '-';
  put_digits(out, unsigned(ymd.month()), 2);
  out += '-';
  put_digits(out, unsigned(ymd.day()), 2);
}

void write_datetime(std::string& out, std::int64_t unix_seconds, std::uint64_t fraction, int digits) {
  std::int64_t days = unix_seconds >= 0 ? unix_seconds / 86400 : -((-unix_seconds + 86399) / 86400);
  std::int64_t sod = unix_seconds - days * 86400;
  write_date(out, days);
  out += 'T';
  put_digits(out, sod / 3600, 2);
  out += ':';
  put_digits(out, sod / 60 % 60, 2);
  out += ':';
  put_digits(out, sod % 60, 2);
  if (digits > 0) {
    out += '.';
    put_digits(out, std::int64_t(fraction), digits);
  }
  out += 'Z';
}

bool parse_date(std::string_view s, std::int64_t& days) { return s.size() == 10 && date_part(s, days); }

bool parse_datetime(std::string_view s, std::int64_t& unix_seconds, std::uint32_t& nanos) {
  // RFC 3339 section 5.6: full-date "T" partial-time time-offset; "t", "z" and a
  // space for "T" are allowed too. No leap seconds: sys_time cannot hold them.
  std::int64_t days;
  int hh, mm, ss;
  if (!date_part(s, days) || s.size() < 20) return false;
  if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') return false;
  if (!fixed_digits(s, 11, 2, hh) || s[13] != ':' || !fixed_digits(s, 14, 2, mm) || s[16] != ':' ||
      !fixed_digits(s, 17, 2, ss))
    return false;
  if (hh > 23 || mm > 59 || ss > 59) return false;
  std::size_t i = 19;
  nanos = 0;
  if (s[i] == '.') {
    ++i;
    int n = 0;
    while (i < s.size() && digit(s[i])) {
      if (n < 9) nanos = nanos * 10 + std::uint32_t(s[i] - '0');
      ++n, ++i;
    }
    if (n == 0) return false;
    for (; n < 9; ++n) nanos *= 10;
  }
  std::int64_t offset = 0;
  if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
    ++i;
  } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    int oh, om;
    if (!fixed_digits(s, i + 1, 2, oh) || i + 3 >= s.size() || s[i + 3] != ':' || !fixed_digits(s, i + 4, 2, om) ||
        oh > 23 || om > 59)
      return false;
    offset = (s[i] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    i += 6;
  } else {
    return false;
  }
  if (i != s.size()) return false;
  unix_seconds = days * 86400 + hh * 3600 + mm * 60 + ss - offset;
  return true;
}

}  // namespace detail
}  // namespace crocket::json

// ---- regular expressions -------------------------------------------------------------

namespace crocket::detail::regex {

bool search(const Program& prog, std::string_view s) {
  const Inst* code = prog.code;
  std::vector<std::uint32_t> cur, next, stack;
  std::vector<std::size_t> mark(prog.size, SIZE_MAX);
  cur.reserve(prog.size);
  next.reserve(prog.size);

  // Adds the thread at `pc` and everything reachable without consuming input.
  auto add = [&](std::vector<std::uint32_t>& list, std::uint32_t pc, std::size_t pos, std::size_t step) {
    stack.push_back(pc);
    while (!stack.empty()) {
      pc = stack.back();
      stack.pop_back();
      if (mark[pc] == step) continue;
      mark[pc] = step;
      const Inst& in = code[pc];
      switch (in.op) {
        case Op::Jmp: stack.push_back(in.x); break;
        case Op::Split: stack.push_back(in.y); stack.push_back(in.x); break;
        case Op::Bol: if (pos == 0) stack.push_back(pc + 1); break;
        case Op::Eol: if (pos == s.size()) stack.push_back(pc + 1); break;
        default: list.push_back(pc);
      }
    }
  };

  std::size_t pos = 0, step = 0;
  add(cur, 0, 0, step);
  while (true) {
    for (auto pc : cur)
      if (code[pc].op == Op::Match) return true;
    if (pos >= s.size()) return false;
    auto [cp, len] = json::decode_utf8(s, pos);
    std::size_t npos = pos + len;
    ++step;
    next.clear();
    for (auto pc : cur) {
      const Inst& in = code[pc];
      bool ok = false;
      switch (in.op) {
        case Op::Char: ok = cp == in.x; break;
        case Op::Any: ok = cp != '\n' && cp != '\r' && cp != 0x2028 && cp != 0x2029; break;
        case Op::Class: {
          for (std::uint32_t i = 0; i < in.y && !ok; ++i)
            ok = cp >= prog.ranges[in.x + i].lo && cp <= prog.ranges[in.x + i].hi;
          ok = ok != in.negate;
          break;
        }
        default: break;
      }
      if (ok) add(next, pc + 1, npos, step);
    }
    add(next, 0, npos, step);  // a match may start at any position
    std::swap(cur, next);
    pos = npos;
  }
}

}  // namespace crocket::detail::regex

namespace crocket::json {

// ---- Reader --------------------------------------------------------------------------

bool Reader::fail(Errc code, const Path* path, std::string message) {
  if (failed_) return false;
  failed_ = true;
  error_.code = code;
  error_.message = std::move(message);
  error_.pointer = detail::pointer(path);
  error_.line = 1;
  error_.column = 1;
  for (const char* c = begin_; c < p_ && c < end_; ++c) {
    if (*c == '\n') ++error_.line, error_.column = 1;
    else if ((static_cast<unsigned char>(*c) & 0xC0) != 0x80) ++error_.column;  // count code points
  }
  return false;
}

bool Reader::mismatch(const Path* path, std::string_view want) {
  // Only call it a type error if the value is valid JSON: `tru` is a syntax error.
  auto start = mark();
  if (detail::kind_at(peek()) != 0 && !skip(path)) return false;
  restore(start);
  std::string_view got;
  switch (detail::kind_at(peek())) {
    case detail::k_null: got = "null"; break;
    case detail::k_bool: got = "boolean"; break;
    case detail::k_number: got = "number"; break;
    case detail::k_string: got = "string"; break;
    case detail::k_array: got = "array"; break;
    case detail::k_object: got = "object"; break;
    default: return fail(Errc::syntax, path, at_end() ? "unexpected end of input" : "expected a value");
  }
  return fail(Errc::type, path, "expected " + std::string(want) + ", got " + std::string(got));
}

bool Reader::backtrack(const Mark& m, const Path* path) {
  backtracked_ += std::size_t(p_ - m.p);
  restore(m);
  if (backtracked_ > 8 * std::size_t(end_ - begin_) + 4096)
    return fail(Errc::limit, path, "untagged std::variant needed too much backtracking; tag it with json::tag");
  return true;
}

bool Reader::literal(std::string_view lit, const Path* path) {
  if (std::string_view(p_, end_ - p_).substr(0, lit.size()) != lit) return fail(Errc::syntax, path, "invalid literal");
  p_ += lit.size();
  return true;
}

bool Reader::null(const Path* path) {
  if (peek() != 'n') return mismatch(path, "null");
  return literal("null", path);
}

bool Reader::boolean(bool& out, const Path* path) {
  if (peek() == 't') { out = true; return literal("true", path); }
  if (peek() == 'f') { out = false; return literal("false", path); }
  return mismatch(path, "boolean");
}

bool Reader::string(std::string_view& out, const Path* path) {
  ++p_;  // '"'
  const char* start = p_;
  bool copied = false;  // escapes seen: the value is being built in scratch_
  auto limit = [&](std::size_t n) {
    return n > opts_.max_string_bytes
               ? fail(Errc::limit, path, "string longer than " + std::to_string(opts_.max_string_bytes) + " bytes")
               : true;
  };
  while (true) {
    // The common case: a run of bytes that need no attention.
    const char* run = p_;
    while (p_ != end_) {
      auto c = static_cast<unsigned char>(*p_);
      if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
      ++p_;
    }
    if (copied) scratch_.append(run, p_);
    if (p_ == end_) return fail(Errc::syntax, path, "unterminated string");
    auto c = static_cast<unsigned char>(*p_);
    if (c == '"') {
      out = copied ? std::string_view(scratch_) : std::string_view(start, p_ - start);
      ++p_;
      return limit(out.size());
    }
    if (c < 0x20) return fail(Errc::syntax, path, "control character in string");
    if (c >= 0x80) {
      std::size_t n = utf8_sequence(reinterpret_cast<const unsigned char*>(p_), reinterpret_cast<const unsigned char*>(end_));
      if (n == 0) return fail(Errc::syntax, path, "invalid UTF-8 in string");
      if (copied) scratch_.append(p_, n);
      p_ += n;
      continue;
    }
    // A backslash: switch to building the value in scratch_.
    if (!copied) {
      scratch_.assign(start, p_);
      copied = true;
    }
    if (scratch_.size() > opts_.max_string_bytes) return limit(scratch_.size());
    if (++p_ == end_) return fail(Errc::syntax, path, "unterminated escape");
    switch (*p_++) {
      case '"': scratch_ += '"'; break;
      case '\\': scratch_ += '\\'; break;
      case '/': scratch_ += '/'; break;
      case 'b': scratch_ += '\b'; break;
      case 'f': scratch_ += '\f'; break;
      case 'n': scratch_ += '\n'; break;
      case 'r': scratch_ += '\r'; break;
      case 't': scratch_ += '\t'; break;
      case 'u': {
        auto hex4 = [&](std::uint32_t& cp) {
          if (end_ - p_ < 4) return fail(Errc::syntax, path, "truncated \\u escape");
          cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p_++;
            cp <<= 4;
            if (digit(h)) cp |= std::uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= std::uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= std::uint32_t(h - 'A' + 10);
            else return fail(Errc::syntax, path, "invalid hex digit in \\u escape");
          }
          return true;
        };
        std::uint32_t cp;
        if (!hex4(cp)) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return fail(Errc::syntax, path, "unpaired surrogate");
          p_ += 2;
          std::uint32_t lo;
          if (!hex4(lo)) return false;
          if (lo < 0xDC00 || lo > 0xDFFF) return fail(Errc::syntax, path, "invalid low surrogate");
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return fail(Errc::syntax, path, "unpaired surrogate");
        }
        put_utf8(scratch_, cp);
        break;
      }
      default: --p_; return fail(Errc::syntax, path, "invalid escape");
    }
  }
}

bool Reader::number(std::string_view& text, bool& integral, const Path* path) {
  const char* start = p_;
  if (p_ != end_ && *p_ == '-') ++p_;
  if (p_ == end_ || !digit(*p_)) return fail(Errc::syntax, path, "invalid number");
  if (*p_ == '0') ++p_;
  else while (p_ != end_ && digit(*p_)) ++p_;
  integral = true;
  if (p_ != end_ && *p_ == '.') {
    integral = false;
    ++p_;
    if (p_ == end_ || !digit(*p_)) return fail(Errc::syntax, path, "invalid number");
    while (p_ != end_ && digit(*p_)) ++p_;
  }
  if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
    integral = false;
    ++p_;
    if (p_ != end_ && (*p_ == '+' || *p_ == '-')) ++p_;
    if (p_ == end_ || !digit(*p_)) return fail(Errc::syntax, path, "invalid number");
    while (p_ != end_ && digit(*p_)) ++p_;
  }
  text = std::string_view(start, p_ - start);
  return true;
}

bool Reader::enter(const Path* path) {
  if (++depth_ > opts_.max_depth)
    return fail(Errc::limit, path, "nested deeper than " + std::to_string(opts_.max_depth) + " levels");
  ++p_;
  return true;
}

bool Reader::begin_object(const Path* path) {
  if (peek() != '{') return mismatch(path, "object");
  return enter(path);
}

bool Reader::begin_array(const Path* path) {
  if (peek() != '[') return mismatch(path, "array");
  return enter(path);
}

int Reader::next_member(std::string_view& key, std::size_t& count, const Path* path) {
  ws();
  if (count == 0 && peek() == '}') {
    ++p_, --depth_;
    return 0;
  }
  if (count > 0) {
    if (peek() == '}') {
      ++p_, --depth_;
      return 0;
    }
    if (peek() != ',') return fail(Errc::syntax, path, at_end() ? "unterminated object" : "expected ',' or '}' in object"), -1;
    ++p_;
    ws();
  }
  if (peek() != '"') return fail(Errc::syntax, path, "expected a string key"), -1;
  if (++count > opts_.max_object_members)
    return fail(Errc::limit, path, "object has more than " + std::to_string(opts_.max_object_members) + " members"), -1;
  if (!string(key, path)) return -1;
  ws();
  if (peek() != ':') return fail(Errc::syntax, path, "expected ':' after key"), -1;
  ++p_;
  ws();
  return 1;
}

int Reader::next_element(std::size_t& count, const Path* path) {
  ws();
  if (peek() == ']' && count == 0) {
    ++p_, --depth_;
    return 0;
  }
  if (count > 0) {
    if (peek() == ']') {
      ++p_, --depth_;
      return 0;
    }
    if (peek() != ',') return fail(Errc::syntax, path, at_end() ? "unterminated array" : "expected ',' or ']' in array"), -1;
    ++p_;
    ws();
  }
  if (++count > opts_.max_array_elements)
    return fail(Errc::limit, path, "array has more than " + std::to_string(opts_.max_array_elements) + " elements"), -1;
  return 1;
}

bool Reader::skip(const Path* path) {
  switch (peek()) {
    case 'n': return literal("null", path);
    case 't': return literal("true", path);
    case 'f': return literal("false", path);
    case '"': {
      std::string_view s;
      return string(s, path);
    }
    case '[': {
      if (!begin_array(path)) return false;
      std::size_t n = 0;
      while (true) {
        int k = next_element(n, path);
        if (k <= 0) return k == 0;
        Path child{path, {}, n - 1, true};
        if (!skip(&child)) return false;
      }
    }
    case '{': {
      if (!begin_object(path)) return false;
      detail::KeySet keys;
      std::string_view key;
      std::size_t n = 0;
      while (true) {
        int k = next_member(key, n, path);
        if (k <= 0) return k == 0;
        if (!keys.insert(key)) return fail(Errc::duplicate_key, path, "duplicate key \"" + std::string(key) + "\"");
        std::string name(key);
        Path child{path, name};
        if (!skip(&child)) return false;
      }
    }
    default: {
      if (detail::kind_at(peek()) != detail::k_number) return mismatch(path, "a value");
      std::string_view text;
      bool integral;
      return number(text, integral, path);
    }
  }
}

// ---- the document ------------------------------------------------------------------

namespace detail {

bool read_value(Reader& r, Value& out, const Path* path) {
  switch (r.peek()) {
    case 'n': out = nullptr; return r.null(path);
    case 't': case 'f': {
      bool b;
      if (!r.boolean(b, path)) return false;
      out = b;
      return true;
    }
    case '"': {
      std::string_view s;
      if (!r.string(s, path)) return false;
      out = std::string(s);
      return true;
    }
    case '[': {
      if (!r.begin_array(path)) return false;
      Array arr;
      std::size_t n = 0;
      while (true) {
        int k = r.next_element(n, path);
        if (k < 0) return false;
        if (k == 0) break;
        Path child{path, {}, n - 1, true};
        if (!read_value(r, arr.emplace_back(), &child)) return false;
      }
      out = std::move(arr);
      return true;
    }
    case '{': {
      if (!r.begin_object(path)) return false;
      Object obj;
      KeySet keys;
      std::string_view key;
      std::size_t n = 0;
      while (true) {
        int k = r.next_member(key, n, path);
        if (k < 0) return false;
        if (k == 0) break;
        if (!keys.insert(key)) return r.fail(Errc::duplicate_key, path, "duplicate key \"" + std::string(key) + "\"");
        auto& [name, v] = obj.emplace_back(std::string(key), Value{});
        Path child{path, name};
        if (!read_value(r, v, &child)) return false;
      }
      out = std::move(obj);
      return true;
    }
    default: {
      if (kind_at(r.peek()) != k_number) return r.mismatch(path, "a value");
      std::string_view text;
      bool integral;
      if (!r.number(text, integral, path)) return false;
      if (integral) {
        std::int64_t i;
        auto res = std::from_chars(text.data(), text.data() + text.size(), i);
        if (res.ec == std::errc()) { out = i; return true; }
      }
      double d;
      auto res = std::from_chars(text.data(), text.data() + text.size(), d);
      if (res.ec != std::errc()) return r.fail(Errc::type, path, "number out of range");
      out = d;
      return true;
    }
  }
}

}  // namespace detail

std::expected<Value, Error> parse(std::string_view text, const ReadOptions& opts) {
  Value v;
  if (auto ok = read(text, v, opts); !ok) return std::unexpected(std::move(ok.error()));
  return v;
}

// ---- writing -------------------------------------------------------------------------

void write_string(std::string& out, std::string_view s) {
  static constexpr char hex[] = "0123456789abcdef";
  out += '"';
  auto p = reinterpret_cast<const unsigned char*>(s.data());
  auto end = p + s.size();
  while (p != end) {
    const unsigned char* run = p;
    while (p != end && *p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\') ++p;
    out.append(reinterpret_cast<const char*>(run), p - run);
    if (p == end) break;
    unsigned char c = *p;
    if (c >= 0x80) {
      std::size_t n = utf8_sequence(p, end);
      if (n == 0) {
        out += "\xEF\xBF\xBD";  // U+FFFD: never emit invalid UTF-8
        ++p;
      } else {
        out.append(reinterpret_cast<const char*>(p), n);
        p += n;
      }
      continue;
    }
    ++p;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 0xF];
    }
  }
  out += '"';
}

void dump(const Value& v, std::string& out) {
  std::visit(
      [&](const auto& x) {
        using X = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<X, std::nullptr_t>) out += "null";
        else if constexpr (std::is_same_v<X, bool>) out += x ? "true" : "false";
        else if constexpr (std::is_same_v<X, std::int64_t> || std::is_same_v<X, double>) encode(out, x);
        else if constexpr (std::is_same_v<X, std::string>) write_string(out, x);
        else if constexpr (std::is_same_v<X, std::shared_ptr<Array>>) {
          out += '[';
          for (std::size_t i = 0; i < x->size(); ++i) { if (i) out += ','; dump((*x)[i], out); }
          out += ']';
        } else {
          out += '{';
          for (std::size_t i = 0; i < x->size(); ++i) {
            if (i) out += ',';
            write_string(out, (*x)[i].first);
            out += ':';
            dump((*x)[i].second, out);
          }
          out += '}';
        }
      },
      v.storage());
}

}  // namespace crocket::json
