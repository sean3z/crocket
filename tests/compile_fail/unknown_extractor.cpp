// A non-path parameter whose type is not an extractor must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Widget { int x; };
namespace bad {
[[= http::get("/w")]]
auto get(Widget w) -> std::string { return std::to_string(w.x); }
}  // namespace bad

int main() { App{}.mount("/", reflect_routes<^^bad>()); }
