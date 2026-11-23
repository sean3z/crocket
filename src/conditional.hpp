#pragma once
// Conditional and range requests (RFC 9110 sections 13 and 14), applied to
// every response before on_response fairings run.

#include "crocket/request.hpp"

namespace crocket::detail {

/// For a 200 to GET or HEAD: adds an ETag (a hash of the body) unless the
/// handler set one or the body is larger than `etag_max_bytes`, answers 304
/// when If-None-Match or If-Modified-Since says the client's copy is current,
/// and serves one byte range as 206 (or 416) when the response has
/// `accept-ranges: bytes`.
void conditional(const Request& rq, Response& rs, std::size_t etag_max_bytes);

/// A strong ETag for `body`, the same 16 hex digits conditional() generates
/// when a handler didn't set one: `"<16 hex>"`.
std::string strong_etag(std::string_view body);

/// If-None-Match, or else If-Modified-Since, says this representation is current.
/// If-None-Match wins when it is present, including when it does not match.
bool fresh_for(const Request& rq, std::string_view etag, std::string_view last_modified);

}  // namespace crocket::detail
