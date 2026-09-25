#include "crocket/json.hpp"

namespace crocket::json {
namespace {

class Parser {
 public:
  Parser(std::string_view s, ParseLimits lim) : s_(s), lim_(lim) {}

  std::expected<Value, ParseError> run() {
    skip_ws();
    Value v;
    if (!value(v, 0)) return std::unexpected(err_);
    skip_ws();
    if (pos_ != s_.size()) {
      fail("unexpected trailing characters");
      return std::unexpected(err_);
    }
    return v;
  }

 private:
  bool fail(std::string msg) {
    if (err_.message.empty()) {
      err_.line = 1;
      err_.column = 1;
      for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
        if (s_[i] == '\n') { ++err_.line; err_.column = 1; }
        else ++err_.column;
      }
      err_.message = std::move(msg);
    }
    return false;
  }
  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool literal(std::string_view lit) {
    if (s_.substr(pos_, lit.size()) != lit) return fail("invalid literal");
    pos_ += lit.size();
    return true;
  }
  static bool digit(char c) { return c >= '0' && c <= '9'; }

  bool value(Value& out, std::size_t depth) {
    if (depth > lim_.max_depth) return fail("nesting too deep");
    if (pos_ >= s_.size()) return fail("unexpected end of input");
    switch (s_[pos_]) {
      case 'n': if (!literal("null")) return false; out = nullptr; return true;
      case 't': if (!literal("true")) return false; out = true; return true;
      case 'f': if (!literal("false")) return false; out = false; return true;
      case '"': { std::string str; if (!string(str)) return false; out = std::move(str); return true; }
      case '[': return array(out, depth);
      case '{': return object(out, depth);
      default: return number(out);
    }
  }

  bool array(Value& out, std::size_t depth) {
    ++pos_;
    Array arr;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; out = std::move(arr); return true; }
    while (true) {
      skip_ws();
      Value v;
      if (!value(v, depth + 1)) return false;
      arr.push_back(std::move(v));
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated array");
      if (s_[pos_] == ',') { ++pos_; continue; }
      if (s_[pos_] == ']') { ++pos_; break; }
      return fail("expected ',' or ']' in array");
    }
    out = std::move(arr);
    return true;
  }

  bool object(Value& out, std::size_t depth) {
    ++pos_;
    Object obj;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; out = std::move(obj); return true; }
    while (true) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected string key in object");
      std::string key;
      if (!string(key)) return false;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':' after object key");
      ++pos_;
      skip_ws();
      Value v;
      if (!value(v, depth + 1)) return false;
      obj.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated object");
      if (s_[pos_] == ',') { ++pos_; continue; }
      if (s_[pos_] == '}') { ++pos_; break; }
      return fail("expected ',' or '}' in object");
    }
    out = std::move(obj);
    return true;
  }

  static void put_utf8(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
  }
  bool hex4(std::uint32_t& cp) {
    if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      cp <<= 4;
      if (digit(c)) cp |= std::uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= std::uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= std::uint32_t(c - 'A' + 10);
      else return fail("invalid hex digit in \\u escape");
    }
    return true;
  }

  bool string(std::string& out) {
    ++pos_;
    while (true) {
      if (pos_ >= s_.size()) return fail("unterminated string");
      char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') { out += c; continue; }
      if (pos_ >= s_.size()) return fail("unterminated escape");
      switch (char e = s_[pos_++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (s_.substr(pos_, 2) != "\\u") return fail("unpaired surrogate");
            pos_ += 2;
            std::uint32_t lo;
            if (!hex4(lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unpaired surrogate");
          }
          put_utf8(out, cp);
          break;
        }
        default: (void)e; return fail("invalid escape");
      }
    }
  }

  bool number(Value& out) {
    std::size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size() || !digit(s_[pos_])) return fail("invalid value");
    if (s_[pos_] == '0') ++pos_;
    else while (pos_ < s_.size() && digit(s_[pos_])) ++pos_;
    bool integral = true;
    if (pos_ < s_.size() && s_[pos_] == '.') {
      integral = false;
      ++pos_;
      if (pos_ >= s_.size() || !digit(s_[pos_])) return fail("invalid number");
      while (pos_ < s_.size() && digit(s_[pos_])) ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (pos_ >= s_.size() || !digit(s_[pos_])) return fail("invalid number");
      while (pos_ < s_.size() && digit(s_[pos_])) ++pos_;
    }
    const char* b = s_.data() + start;
    const char* e = s_.data() + pos_;
    if (integral) {
      std::int64_t i;
      auto r = std::from_chars(b, e, i);
      if (r.ec == std::errc() && r.ptr == e) { out = i; return true; }
    }
    double d;
    auto r = std::from_chars(b, e, d);
    if (r.ec != std::errc() || r.ptr != e) return fail("invalid number");
    out = d;
    return true;
  }

  std::string_view s_;
  ParseLimits lim_;
  std::size_t pos_ = 0;
  ParseError err_;
};

}  // namespace

std::expected<Value, ParseError> parse(std::string_view text, ParseLimits limits) {
  return Parser(text, limits).run();
}

void write_string(std::string& out, std::string_view s) {
  static constexpr char hex[] = "0123456789abcdef";
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out += hex[(c >> 4) & 0xF];
          out += hex[c & 0xF];
        } else {
          out += c;
        }
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
