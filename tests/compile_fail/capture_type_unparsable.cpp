// A capture bound to a type with no FromParam must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Blob { int x; };
namespace bad {
[[= http::get("/blob/{b}")]]
auto get(Blob b) -> std::string { return std::to_string(b.x); }
}  // namespace bad

int main() { Crocket{}.mount("/", reflect_routes<^^bad>()); }
