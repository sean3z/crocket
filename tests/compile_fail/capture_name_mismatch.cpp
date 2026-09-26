// Acceptance 5: `{age}` in the path with a parameter named `years` must not compile,
// and the diagnostic must name both identifiers.
#include <crocket/crocket.hpp>
using namespace crocket;

namespace bad {
[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t years) -> std::string { return std::string(name) + std::to_string(years); }
}  // namespace bad

int main() { Crocket{}.mount("/", reflect_routes<^^bad>()); }
