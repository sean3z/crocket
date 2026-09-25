// Control: the well-formed version of capture_name_mismatch.cpp compiles.
#include <crocket/crocket.hpp>
using namespace crocket;

namespace good {
[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string { return std::string(name) + std::to_string(age); }
}  // namespace good

int main() { App{}.mount("/", reflect_routes<^^good>()); }
