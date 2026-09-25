// The Rocket "hello world", crocket style.
//
//   ./build/crocket_hello
//   curl localhost:8000/hello/Rocketeer/42     -> Hello, 42 year old named Rocketeer!
//   curl -i localhost:8000/hello/Rocketeer/400 -> 404 path.invalid (handler never runs)

#include <crocket/crocket.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

using namespace crocket;

namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  return std::format("Hello, {} year old named {}!", age, name);
}

[[= http::get("/")]]
auto index() -> std::string { return "Try GET /hello/<name>/<age>\n"; }

}  // namespace api

int main() {
  // CROCKET_PROFILE=dev: readable logs, a route banner, error details, 127.0.0.1.
  return App{Config::from_env()}
      .attach(Logger{})
      .mount("/", reflect_routes<^^api>())
      .listen({.port = 8000});
}
