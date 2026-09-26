// [[= json::from_path]] on a non-optional member must not compile: the field is
// absent on routes without a matching capture (e.g. POST).
#include <crocket/crocket.hpp>
using namespace crocket;

struct Widget {
  [[= json::from_path]] std::uint64_t id;
  std::string name;
};
namespace bad {
[[= http::put("/widgets/{id}")]]
auto put(std::uint64_t id, Json<Widget> w) -> std::string { return w->name + std::to_string(id); }
}  // namespace bad

int main() { Crocket{}.mount("/", reflect_routes<^^bad>()); }
