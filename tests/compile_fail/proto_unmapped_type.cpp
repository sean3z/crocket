// A message member whose type has no protobuf mapping must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Widget {
  std::string name;
  char grade;  // text, not a number: use std::string
};
namespace bad {
[[= grpc::rpc]]
auto make(Widget w) -> Widget { return w; }
}  // namespace bad

int main() { Crocket{}.mount("shop", reflect_routes<^^bad>(), Mode::Grpc); }
