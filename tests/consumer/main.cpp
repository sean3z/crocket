// A minimal application: one route, one request through LocalClient.
#include <crocket/crocket.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace crocket;

namespace api {
[[= http::get("/hi/{name}")]]
auto hi(std::string_view name) -> std::string { return "hi " + std::string(name); }
}  // namespace api

int main() {
  App app;
  app.mount("/", reflect_routes<^^api>());
  LocalClient client(app);
  auto r = client.get("/hi/consumer").dispatch();
  std::printf("%d %s\n", r.status, r.body.c_str());
  return r.status == 200 && r.body == "hi consumer" ? 0 : 1;
}
