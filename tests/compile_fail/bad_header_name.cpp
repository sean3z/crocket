// A Header<"..."> whose name is not an HTTP token must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

namespace bad {
[[= http::get("/h")]]
auto get(Header<"x tenant"> t) -> std::string { return *t; }
}  // namespace bad

int main() { Crocket{}.mount("/", reflect_routes<^^bad>()); }
