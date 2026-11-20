// A tiny frontend from .files(): embedded in the binary, or a directory.
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
  StaticFiles files{.prefix = "/"};
  if (const char* root = std::getenv("WEB_ROOT")) files.root = root;
  else files.assets = {asset("index.html", index_html), asset("app.js", app_js), asset("style.css", style_css)};
  // Shield's default policy is default-src 'none', which blocks this page's
  // script and stylesheet. 'self' lets the page load what it serves.
  return build(Config::from_env())
      .attach(Logger{})
      .attach(Shield{}.set("content-security-policy", "default-src 'self'"))
      .files(std::move(files))
      .mount("/", reflect_routes<^^api>())
      .launch();
}
