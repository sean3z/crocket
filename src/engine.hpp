#pragma once
// Private seam between Crocket and the wire engine. Only engine_h2o.cpp includes
// h2o.
#include "crocket/app.hpp"

namespace crocket::detail {
/// Serve until a stop signal, drain, then return an exit status.
int run_engine(Crocket& app, const LaunchOptions& opts);
/// on_shutdown fairings (reverse attach order), then drop managed state.
void shut_down(Crocket& app);
}  // namespace crocket::detail
