#pragma once
// Private seam between App and the wire engine. Only engine_lws.cpp includes
// libwebsockets.
#include "crocket/app.hpp"

namespace crocket::detail {
/// Serve until a stop signal, drain, then return an exit status.
int run_engine(App& app, const ListenOptions& opts);
/// on_shutdown fairings (reverse attach order), then drop managed state.
void shutdown_app(App& app);
}  // namespace crocket::detail
