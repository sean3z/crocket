// Pinning a field to a number an earlier member already has must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Widget {
  std::string name;                          // 1
  std::string size;                          // 2
  [[= proto::field(2)]] std::string colour;  // 2 again
};
namespace bad {
[[= grpc::rpc]]
auto make(Widget w) -> Widget { return w; }
}  // namespace bad

int main() { Crocket{}.mount("shop", reflect_routes<^^bad>(), Mode::Grpc); }
