// A malformed route template is rejected where the annotation is written.
#include <crocket/crocket.hpp>
using namespace crocket;

namespace bad {
[[= http::get("/users/{id")]]
auto get(int id) -> std::string { return std::to_string(id); }
}  // namespace bad

int main() { Crocket{}.mount("/", reflect_routes<^^bad>()); }
