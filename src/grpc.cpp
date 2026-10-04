#include "crocket/grpc.hpp"
#include "crocket/app.hpp"

#include <charconv>

namespace crocket::grpc {

int http_status(Code c) {
  switch (c) {
    case Code::Ok: return 200;
    case Code::Cancelled: return 499;
    case Code::InvalidArgument:
    case Code::FailedPrecondition:
    case Code::OutOfRange: return 400;
    case Code::DeadlineExceeded: return 504;
    case Code::NotFound: return 404;
    case Code::AlreadyExists:
    case Code::Aborted: return 409;
    case Code::PermissionDenied: return 403;
    case Code::ResourceExhausted: return 429;
    case Code::Unimplemented: return 501;
    case Code::Unavailable: return 503;
    case Code::Unauthenticated: return 401;
    case Code::Unknown:
    case Code::Internal:
    case Code::DataLoss: return 500;
  }
  return 500;
}

Code code_of(const ApiError& e) {
  if (e.grpc_status >= 0 && e.grpc_status <= 16) return static_cast<Code>(e.grpc_status);
  switch (e.status) {
    case 400:
    case 411:
    case 422: return Code::InvalidArgument;
    case 401: return Code::Unauthenticated;
    case 403: return Code::PermissionDenied;
    case 404: return Code::NotFound;
    case 409: return Code::AlreadyExists;
    case 412: return Code::FailedPrecondition;
    case 413:
    case 429: return Code::ResourceExhausted;
    case 499: return Code::Cancelled;
    case 501: return Code::Unimplemented;
    case 503: return Code::Unavailable;
    case 504: return Code::DeadlineExceeded;
    default: return e.status >= 500 ? Code::Internal : e.status >= 400 ? Code::FailedPrecondition : Code::Unknown;
  }
}

bool is_grpc_content_type(std::string_view ct) {
  constexpr std::string_view base = "application/grpc";
  if (ct.size() < base.size() || !Headers::iequals(ct.substr(0, base.size()), base)) return false;
  return ct.size() == base.size() || ct[base.size()] == '+' || ct[base.size()] == ';';
}

bool is_grpc_request(const Request& rq) {
  auto ct = rq.header("content-type");
  return ct && is_grpc_content_type(*ct);
}

std::string frame(std::string_view message) {
  auto n = static_cast<std::uint32_t>(message.size());
  std::string out;
  out.reserve(5 + message.size());
  out += '\0';
  for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((n >> shift) & 0xff);
  out += message;
  return out;
}

namespace {

/// grpc-message is percent-encoded: bytes outside printable ASCII, and '%'.
std::string percent_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x20 && c <= 0x7e && c != '%') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xf];
    }
  }
  return out;
}

/// The message in a body of exactly one length-prefixed message.
std::expected<std::string_view, std::string> one_message(std::string_view body) {
  if (body.size() < 5) return std::unexpected("body is not a length-prefixed gRPC message");
  if (body[0] != 0) return std::unexpected("compressed");
  std::uint32_t n = 0;
  for (int i = 1; i <= 4; ++i) n = (n << 8) | static_cast<unsigned char>(body[i]);
  if (n != body.size() - 5)
    return std::unexpected("message length " + std::to_string(n) + " does not match the " +
                           std::to_string(body.size() - 5) + " bytes sent (a unary call carries one message)");
  return body.substr(5);
}

}  // namespace

Status status_of(const Response& rs) {
  auto st = rs.trailers.get("grpc-status");
  auto msg = rs.trailers.get("grpc-message");
  if (!st) {  // trailers-only response
    st = rs.headers.get("grpc-status");
    msg = rs.headers.get("grpc-message");
  }
  if (!st) return {Code::Internal, "response has no grpc-status"};
  int code = 0;
  auto [p, ec] = std::from_chars(st->data(), st->data() + st->size(), code);
  if (ec != std::errc{} || p != st->data() + st->size() || code < 0 || code > 16)
    return {Code::Unknown, "invalid grpc-status '" + std::string(*st) + "'"};
  return {static_cast<Code>(code), msg ? crocket::detail::percent_decode(*msg, false) : std::string()};
}

namespace detail {

std::expected<std::string_view, ApiError> unframe(const Request& rq) {
  auto ct = rq.header("content-type");
  if (!ct || !is_grpc_content_type(*ct))
    return std::unexpected(ApiError{415, "grpc.content_type", "expected content-type application/grpc",
                                    ct ? std::string(*ct) : std::string("no content-type")});
  auto m = one_message(rq.body);
  if (m) return *m;
  if (m.error() == "compressed")
    return std::unexpected(error(Code::Unimplemented, "grpc.compression",
                                 "compressed messages are not supported; use grpc-encoding identity"));
  return std::unexpected(error(Code::InvalidArgument, "grpc.framing", m.error()));
}

std::expected<std::string_view, Status> unframe_reply(const Response& rs) {
  auto m = one_message(rs.body);
  if (!m) return std::unexpected(Status{Code::Internal, "reply: " + m.error()});
  return *m;
}

void write_reply(std::string_view message, Response& rs) {
  rs.status = 200;
  rs.set_content_type("application/grpc");
  rs.body = frame(message);
  rs.trailers = {};
  rs.trailers.set("grpc-status", "0");
}

void write_status(const ApiError& e, const Request& rq, Response& rs) {
  rs.status = e.status;  // the HTTP equivalent, for logs and metrics; the engine sends 200
  rs.error_code = e.code;
  rs.error_detail = e.detail;
  rs.body.clear();
  rs.trailers = {};
  rs.set_content_type("application/grpc");
  rs.headers.set("grpc-status", std::to_string(static_cast<int>(code_of(e))));
  std::string msg = e.message;
  if (rq.dev_profile && !e.detail.empty()) msg += " (" + e.detail + ")";
  rs.headers.set("grpc-message", percent_encode(msg));
  rs.headers.set("crocket-error-code", e.code);  // the stable dotted code, as custom metadata
}

}  // namespace detail
}  // namespace crocket::grpc
