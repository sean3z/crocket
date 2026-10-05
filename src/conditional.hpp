#pragma once
// Conditional and range requests (RFC 9110 sections 13 and 14), applied to
// every response before on_response fairings run.

#include "crocket/request.hpp"

namespace crocket::detail {

/// For a 200 to GET or HEAD: adds an ETag (a hash of the body) unless the
/// handler set one, answers 304 when If-None-Match or If-Modified-Since says
/// the client's copy is current, and serves one byte range as 206 (or 416)
/// when the response has `accept-ranges: bytes`.
void conditional(const Request& rq, Response& rs);

}  // namespace crocket::detail
