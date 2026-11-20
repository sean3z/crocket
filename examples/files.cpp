// A tiny frontend from .mount(..., files{...}): embedded in the binary, or a directory.
//
//   ./build/crocket_files
//   curl localhost:8000/                 # index.html
//   curl localhost:8000/app.js
//   curl localhost:8000/api/health       # a route, not a file
//
//   WEB_ROOT=examples/web ./build/crocket_files
//
// WEB_ROOT is this program's directory. CROCKET_PORT and the other CROCKET_*
// variables still belong to crocket (see Config::from_env).

#include <crocket/crocket.hpp>

#include <cstdlib>
#include <string>

using namespace crocket;

namespace api {

[[= http::get("/api/health")]]
auto health() -> std::string { return "ok\n"; }

}  // namespace api

static constexpr unsigned char index_html[] = {
#embed "web/index.html"
};
static constexpr unsigned char app_js[] = {
#embed "web/app.js"
};
static constexpr unsigned char style_css[] = {
#embed "web/style.css"
};

int main() {
  files web;
  if (const char* root = std::getenv("WEB_ROOT")) web.root = root;
  else web.assets = {asset("index.html", index_html), asset("app.js", app_js), asset("style.css", style_css)};
  // Shield's default policy is default-src 'none', which would block this page's
  // script and stylesheet. The html rule replaces that one header. The route
  // wins whether it's mounted before or after the files.
  web.cache = {{"*.html", "no-cache"}};
  web.headers = {{"*.html", {{"content-security-policy", "default-src 'self'"}}}};
  return build(Config::from_env())
      .attach(Logger{})
      .mount("/", std::move(web))
      .mount("/", reflect_routes<^^api>())
      .launch();
}
