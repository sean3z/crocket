#pragma once
// Application logging: levelled, structured, and tagged with the request it
// belongs to without passing anything around.
//
//   log::info("created user {user_id} on plan {plan}", user.id, plan);
//
// writes
//
//   {"ts":"…","level":"info","msg":"created user 42 on plan pro","request_id":"5f0c…",
//    "trace_id":"4bf9…","route":"/users","handler":"api::create","subject":"alice",
//    "user_id":42,"plan":"pro"}
//
// Each {name} in the message is a field as well as a place in the text; the
// number of placeholders must match the arguments, which is checked when the
// program compiles. Arguments may be anything JSON can encode, structs included.
// While crocket runs a handler (and fairings), the lines carry that request's
// context; elsewhere they carry none. Config::log sets the level, sampling,
// redaction and where lines go. Lines are written by a background thread.

#include "crocket/json.hpp"

#include <meta>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crocket {
struct Request;

namespace log {

enum class Level : std::uint8_t { debug, info, warn, error };

/// What to do with a line when the writer has fallen behind and the buffer is full.
enum class WhenFull : std::uint8_t {
  drop,   // discard it (counted, reported in a later line and in Metrics)
  block,  // wait for room: never lose a line, but a slow sink slows requests
};

/// Config::log.
struct Options {
  Level level = Level::info;  // lines below it are skipped; Config::dev() uses debug
  /// Share of request lines (Logger) kept for responses below 400, decided per
  /// request id. Errors are always kept.
  double sample = 1.0;
  /// Fields whose name contains one of these (ignoring case) are written as
  /// "[redacted]", in the message too, and inside structs.
  std::vector<std::string> redact = {"password", "secret", "token", "authorization", "cookie", "api_key"};
  std::size_t buffer = 10'000;  // lines waiting for the writer
  WhenFull when_full = WhenFull::drop;
  /// Where lines go, one call per line, from the writer thread. Empty: stderr.
  std::function<void(std::string_view line)> sink = {};
};

namespace detail {

class Hub;

/// A "{name}" message, parsed when the program compiles.
template <std::size_t N>
struct Parsed {
  std::array<std::string_view, N> names{};
};

consteval bool reserved(std::string_view name) {
  for (std::string_view r : {"ts", "level", "msg", "request_id", "trace_id", "route", "handler", "subject"})
    if (name == r) return true;
  return false;
}

template <std::size_t N>
consteval Parsed<N> parse(std::string_view text) {
  Parsed<N> out;
  std::size_t count = 0;
  auto fail = [&](std::string why) {
    std::string msg = "crocket: log message \"" + std::string(text) + "\": " + why;
    throw std::meta::exception(std::u8string(msg.begin(), msg.end()), ^^Parsed<N>);
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '}') {
      if (i + 1 < text.size() && text[i + 1] == '}') { ++i; continue; }
      fail("unmatched '}' (write '}}' for a brace)");
    }
    if (c != '{') continue;
    if (i + 1 < text.size() && text[i + 1] == '{') { ++i; continue; }
    auto close = text.find('}', i);
    if (close == std::string_view::npos) fail("unmatched '{' (write '{{' for a brace)");
    auto name = text.substr(i + 1, close - i - 1);
    bool ok = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
    for (char n : name)
      ok = ok && ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9') || n == '_');
    if (!ok) fail("placeholder {" + std::string(name) + "} must be a name of letters, digits and '_'");
    if (reserved(name)) fail("placeholder {" + std::string(name) + "} is a field crocket writes itself; rename it");
    for (std::size_t k = 0; k < count && k < N; ++k)
      if (out.names[k] == name) fail("placeholder {" + std::string(name) + "} appears twice");
    if (count < N) out.names[count] = name;
    ++count;
    i = close;
  }
  if (count != N)
    fail("has " + crocket::detail::decimal(static_cast<long long>(count)) + " placeholder" + (count == 1 ? "" : "s") +
         " but " + crocket::detail::decimal(static_cast<long long>(N)) +
         " argument" + (N == 1 ? "" : "s") + " follow");
  return out;
}

/// A value for one placeholder: JSON for the field, and its text in the message.
struct Field {
  std::string_view name;
  std::string json;
  std::optional<std::string_view> text;  // a string argument, shown as itself in the message
};

/// The hub that lines go to: the running request's app, else the last app that
/// ignited, else stderr. False if `level` is below its threshold.
Hub* hub_for(Level level);
void emit(Hub* hub, Level level, std::string_view text, std::span<Field> fields);

template <class T>
Field field(std::string_view name, const T& v) {
  using U = std::remove_cvref_t<T>;
  static_assert(json::Encodable<U>, "crocket: a log argument must be JSON-encodable (a number, string, struct, ...)");
  Field f{name, {}, std::nullopt};
  json::encode(f.json, v);
  if constexpr (std::is_convertible_v<const U&, std::string_view>) f.text = std::string_view(v);
  return f;
}

}  // namespace detail

/// The message of a log call: a string literal whose {placeholders} match the arguments.
template <class... Args>
struct Message {
  template <class S>
    requires std::convertible_to<const S&, std::string_view>
  consteval Message(const S& s)  // NOLINT: implicit, like std::format_string
      : text(s), names(detail::parse<sizeof...(Args)>(text).names) {}
  std::string_view text;
  std::array<std::string_view, sizeof...(Args)> names;
};

template <class... Args>
void write(Level level, const Message<std::type_identity_t<Args>...>& m, const Args&... args) {
  detail::Hub* hub = detail::hub_for(level);
  if (!hub) return;
  std::array<detail::Field, sizeof...(Args)> fields;
  std::size_t i = 0;
  ((fields[i] = detail::field(m.names[i], args), ++i), ...);
  detail::emit(hub, level, m.text, fields);
}

template <class... Args>
void debug(Message<std::type_identity_t<Args>...> m, const Args&... args) { write(Level::debug, m, args...); }
template <class... Args>
void info(Message<std::type_identity_t<Args>...> m, const Args&... args) { write(Level::info, m, args...); }
template <class... Args>
void warn(Message<std::type_identity_t<Args>...> m, const Args&... args) { write(Level::warn, m, args...); }
template <class... Args>
void error(Message<std::type_identity_t<Args>...> m, const Args&... args) { write(Level::error, m, args...); }

/// The current request's context, kept after the request ends, to carry into a
/// thread the handler starts: `auto ctx = log::context();` then, on that
/// thread, `log::Scope scope{ctx};`.
class Context {
 public:
  Context() = default;

 private:
  friend class Scope;
  friend Context context();
  friend void detail::emit(detail::Hub*, Level, std::string_view, std::span<detail::Field>);
  friend detail::Hub* detail::hub_for(Level);
  std::shared_ptr<detail::Hub> hub_;
  std::string request_id_, trace_id_, route_, handler_, subject_;
};
Context context();

/// Makes `ctx` the context of this thread's log lines until the scope ends.
class Scope {
 public:
  explicit Scope(const Context& ctx);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const Context* prev_ctx_;
  const Request* prev_rq_;
  const std::shared_ptr<detail::Hub>* prev_hub_;
};

}  // namespace log
}  // namespace crocket
