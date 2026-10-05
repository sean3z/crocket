#pragma once
// The regular expressions behind [[= json::pattern("...")]]. A pattern is
// compiled during compilation, so a malformed one is a compile error, into a
// program that a Pike VM (src/json.cpp) runs in time linear in the input, with
// no backtracking and no recursion: no request body can make it slow or blow
// the stack, as std::regex can.
//
// The syntax is a subset of ECMAScript's: literals; `.`; classes `[a-z_]` and
// `[^...]`; `\d \w \s` (and `\D \W \S` outside classes); `^` and `$`; groups
// `(...)` and `(?:...)`; `|`; and the quantifiers `* + ? {n} {n,} {n,m}`, with
// an optional trailing `?` (laziness does not change whether a string matches).
// Backreferences, lookaround and flags are not supported.
//
// As in JSON Schema, a pattern matches if it matches anywhere in the string:
// anchor it, "^[a-z]+$", to constrain the whole value. `.` and classes match
// code points, not bytes.

#include <meta>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crocket::detail::regex {

enum class Op : std::uint8_t { Char, Any, Class, Split, Jmp, Bol, Eol, Match };

/// One VM instruction. Char: code point `x`. Class: ranges [x, x + y), inverted
/// when `negate`. Split: continue at both `x` and `y`. Jmp: continue at `x`.
struct Inst {
  Op op = Op::Match;
  bool negate = false;
  std::uint32_t x = 0, y = 0;
};

struct Range {
  char32_t lo = 0, hi = 0;
};

/// A compiled pattern, in static storage.
struct Program {
  const Inst* code;
  std::uint32_t size;
  const Range* ranges;
};

/// True if `prog` matches somewhere in `s` (UTF-8; invalid bytes match as U+FFFD).
bool search(const Program& prog, std::string_view s);

inline constexpr std::size_t kMaxProgram = 4096;

struct Compiled {
  std::vector<Inst> code;
  std::vector<Range> ranges;
};

/// Pattern source to program. Throws std::meta::exception, naming the pattern
/// and the offset, on a syntax error.
class Compiler {
 public:
  consteval explicit Compiler(std::string_view src) : src_(src) {}

  consteval Compiled run() {
    int root = alternation();
    if (pos_ < src_.size()) error("unmatched ')'");
    emit(root);
    push({Op::Match});
    return {std::move(code_), std::move(ranges_)};
  }

 private:
  enum class K : std::uint8_t { Empty, Char, Any, Class, Bol, Eol, Cat, Alt, Repeat };
  struct Node {
    K k = K::Empty;
    char32_t c = 0;
    bool negate = false;
    std::uint32_t lo = 0, n = 0;   // Class: ranges [lo, lo + n)
    int a = -1, b = -1;            // children
    std::uint32_t min = 0, max = 0;
    bool unbounded = false;
  };

  static consteval std::string decimal(std::size_t v) {
    std::string d;
    do d.insert(d.begin(), char('0' + v % 10)); while (v /= 10);
    return d;
  }
  [[noreturn]] consteval void error(std::string_view what) const {
    std::string msg = "crocket: invalid json::pattern \"" + std::string(src_) + "\": " + std::string(what) +
                      " at offset " + decimal(pos_);
    throw std::meta::exception(std::u8string(msg.begin(), msg.end()), ^^Compiler);
  }
  consteval int add(Node n) {
    nodes_.push_back(n);
    return int(nodes_.size()) - 1;
  }
  consteval bool more() const { return pos_ < src_.size(); }
  consteval char peek() const { return src_[pos_]; }

  consteval int alternation() {
    int left = sequence();
    while (more() && peek() == '|') {
      ++pos_;
      int right = sequence();
      left = add({.k = K::Alt, .a = left, .b = right});
    }
    return left;
  }

  consteval int sequence() {
    int left = add({.k = K::Empty});
    while (more() && peek() != '|' && peek() != ')') left = add({.k = K::Cat, .a = left, .b = quantified()});
    return left;
  }

  consteval bool quantifier_next() const {
    return more() && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{');
  }

  consteval std::uint32_t count() {
    if (!more() || peek() < '0' || peek() > '9') error("expected a number in {}");
    std::uint32_t v = 0;
    while (more() && peek() >= '0' && peek() <= '9') {
      v = v * 10 + std::uint32_t(peek() - '0');
      if (v > 1000) error("repetition count above 1000");
      ++pos_;
    }
    return v;
  }

  consteval int quantified() {
    int atom_node = atom();
    if (!quantifier_next()) return atom_node;
    Node r{.k = K::Repeat, .a = atom_node};
    switch (src_[pos_++]) {
      case '*': r.unbounded = true; break;
      case '+': r.min = 1; r.unbounded = true; break;
      case '?': r.max = 1; break;
      default: {  // '{'
        r.min = r.max = count();
        if (more() && peek() == ',') {
          ++pos_;
          if (more() && peek() == '}') r.unbounded = true;
          else r.max = count();
        }
        if (!more() || peek() != '}') error("expected '}'");
        ++pos_;
        if (!r.unbounded && r.max < r.min) error("{n,m} with m < n");
      }
    }
    if (more() && peek() == '?') ++pos_;  // lazy: same language
    if (quantifier_next()) error("nothing to repeat");
    return add(r);
  }

  consteval char32_t utf8() {
    auto b = static_cast<unsigned char>(src_[pos_++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) error("invalid UTF-8");
    char32_t cp = b & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
      if (!more() || (static_cast<unsigned char>(peek()) & 0xC0) != 0x80) error("invalid UTF-8");
      cp = (cp << 6) | (static_cast<unsigned char>(src_[pos_++]) & 0x3F);
    }
    return cp;
  }

  consteval char32_t hex(int digits) {
    char32_t v = 0;
    for (int i = 0; i < digits; ++i) {
      if (!more()) error("truncated hex escape");
      char c = src_[pos_++];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) error("invalid hex escape");
      v = v * 16 + char32_t(d);
    }
    return v;
  }

  // \d \w \s as ranges appended to ranges_; returns false for other letters.
  consteval bool shorthand(char c) {
    switch (c) {
      case 'd': ranges_.push_back({'0', '9'}); return true;
      case 'w':
        ranges_.push_back({'0', '9'});
        ranges_.push_back({'A', 'Z'});
        ranges_.push_back({'_', '_'});
        ranges_.push_back({'a', 'z'});
        return true;
      case 's':
        for (Range r : {Range{'\t', '\r'}, Range{' ', ' '}, Range{0xA0, 0xA0}, Range{0x1680, 0x1680},
                        Range{0x2000, 0x200A}, Range{0x2028, 0x2029}, Range{0x202F, 0x202F},
                        Range{0x205F, 0x205F}, Range{0x3000, 0x3000}, Range{0xFEFF, 0xFEFF}})
          ranges_.push_back(r);
        return true;
      default: return false;
    }
  }

  // The code point of a single-character escape (after the backslash).
  consteval char32_t escaped_char(char c) {
    switch (c) {
      case 'n': return '\n';
      case 't': return '\t';
      case 'r': return '\r';
      case 'f': return '\f';
      case 'v': return '\v';
      case '0': return 0;
      case 'x': return hex(2);
      case 'u': return hex(4);
      default:
        if (std::string_view("^$\\.*+?()[]{}|/-").find(c) != std::string_view::npos) return char32_t(c);
        --pos_;
        error(std::string("unsupported escape '\\") + c + "'");
    }
  }

  consteval int atom() {
    char c = peek();
    switch (c) {
      case '(': {
        ++pos_;
        if (more() && peek() == '?') {
          if (src_.substr(pos_, 2) != "?:") error("lookaround and named groups are not supported");
          pos_ += 2;
        }
        int inner = alternation();
        if (!more() || peek() != ')') error("missing ')'");
        ++pos_;
        return inner;
      }
      case '[': return char_class();
      case '.': ++pos_; return add({.k = K::Any});
      case '^': ++pos_; return add({.k = K::Bol});
      case '$': ++pos_; return add({.k = K::Eol});
      case '*': case '+': case '?': case '{': error("nothing to repeat");
      case '\\': {
        ++pos_;
        if (!more()) error("trailing backslash");
        char e = src_[pos_++];
        if (e == 'b' || e == 'B') error("word boundaries are not supported");
        if (e >= '1' && e <= '9') error("backreferences are not supported");
        bool upper = e == 'D' || e == 'W' || e == 'S';
        std::uint32_t lo = std::uint32_t(ranges_.size());
        if (shorthand(upper ? char(e - 'A' + 'a') : e))
          return add({.k = K::Class, .negate = upper, .lo = lo, .n = std::uint32_t(ranges_.size()) - lo});
        return add({.k = K::Char, .c = escaped_char(e)});
      }
      default: return add({.k = K::Char, .c = utf8()});
    }
  }

  consteval char32_t class_char() {
    if (peek() != '\\') return utf8();
    ++pos_;
    if (!more()) error("trailing backslash");
    char e = src_[pos_++];
    if (e == 'b') return '\b';
    return escaped_char(e);
  }

  consteval int char_class() {
    ++pos_;  // '['
    bool negate = more() && peek() == '^';
    if (negate) ++pos_;
    std::uint32_t lo = std::uint32_t(ranges_.size());
    while (true) {
      if (!more()) error("missing ']'");
      if (peek() == ']') break;
      if (peek() == '\\' && pos_ + 1 < src_.size()) {
        char e = src_[pos_ + 1];
        if (e == 'D' || e == 'W' || e == 'S') error("\\D, \\W and \\S are not supported inside [...]");
        if (e == 'd' || e == 'w' || e == 's') {
          pos_ += 2;
          shorthand(e);
          continue;
        }
      }
      char32_t first = class_char();
      char32_t last = first;
      if (pos_ + 1 < src_.size() && peek() == '-' && src_[pos_ + 1] != ']') {
        ++pos_;
        last = class_char();
        if (last < first) error("range out of order in [...]");
      }
      ranges_.push_back({first, last});
    }
    ++pos_;  // ']'
    return add({.k = K::Class, .negate = negate, .lo = lo, .n = std::uint32_t(ranges_.size()) - lo});
  }

  consteval std::uint32_t push(Inst i) {
    if (code_.size() >= kMaxProgram) error("pattern too large once repetitions are expanded");
    code_.push_back(i);
    return std::uint32_t(code_.size()) - 1;
  }
  consteval std::uint32_t here() const { return std::uint32_t(code_.size()); }

  consteval void emit(int id) {
    Node n = nodes_[id];
    switch (n.k) {
      case K::Empty: return;
      case K::Char: push({.op = Op::Char, .x = std::uint32_t(n.c)}); return;
      case K::Any: push({.op = Op::Any}); return;
      case K::Class: push({.op = Op::Class, .negate = n.negate, .x = n.lo, .y = n.n}); return;
      case K::Bol: push({.op = Op::Bol}); return;
      case K::Eol: push({.op = Op::Eol}); return;
      case K::Cat: emit(n.a); emit(n.b); return;
      case K::Alt: {
        auto split = push({.op = Op::Split});
        emit(n.a);
        auto jmp = push({.op = Op::Jmp});
        code_[split].x = split + 1;
        code_[split].y = here();
        emit(n.b);
        code_[jmp].x = here();
        return;
      }
      case K::Repeat: {
        for (std::uint32_t i = 0; i < n.min; ++i) emit(n.a);
        if (n.unbounded) {
          auto split = push({.op = Op::Split});
          emit(n.a);
          push({.op = Op::Jmp, .x = split});
          code_[split].x = split + 1;
          code_[split].y = here();
        } else {
          for (std::uint32_t i = n.min; i < n.max; ++i) {
            auto split = push({.op = Op::Split});
            emit(n.a);
            code_[split].x = split + 1;
            code_[split].y = here();
          }
        }
        return;
      }
    }
  }

  std::string_view src_;
  std::size_t pos_ = 0;
  std::vector<Node> nodes_;
  std::vector<Inst> code_;
  std::vector<Range> ranges_;
};

}  // namespace crocket::detail::regex
