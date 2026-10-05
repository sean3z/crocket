#pragma once
// gzip for responses (Config::compress), and the Vary header it and content
// negotiation maintain.

#include "crocket/request.hpp"

#include <string_view>

namespace crocket::detail {

/// Adds `header` to the response's Vary list unless it is there already.
void add_vary(Response& rs, std::string_view header);

/// gzips the body when the client accepts it and the content is text-like and
/// at least 1 KiB, adding Content-Encoding and Vary: Accept-Encoding.
void compress(const Request& rq, Response& rs);

}  // namespace crocket::detail
