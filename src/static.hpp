#pragma once
// Prepared static mounts. The definition of StaticTable lives in static.cpp,
// where its directory descriptors are closed.

#include "crocket/app.hpp"

#include <string>
#include <vector>

namespace crocket::detail {

void prepare_static(Core& core, std::vector<std::string>& errors);

/// GET or HEAD under a disk mount: opening the file can block, so the event
/// loop should hand the request to a worker. A fresh small-file cache hit
/// (release profile only) does not. Embedded mounts are memory.
bool static_wants_worker(const Core& core, http::Method method, std::string_view path,
                         std::string_view accept_encoding);

/// Serves one file into `res` and labels the request. False: the caller writes
/// the ordinary 404. A longest-prefix miss does not consult a shorter mount.
bool serve_static(Core& core, Request& req, Response& res);

}  // namespace crocket::detail
