// A unary RPC takes one request message.
#include <crocket/crocket.hpp>
using namespace crocket;

struct A { int x; };
struct B { int y; };
namespace bad {
[[= grpc::rpc]]
auto both(A a, B b) -> A { return {a.x + b.y}; }
}  // namespace bad

int main() { Crocket{}.mount("shop", reflect_routes<^^bad>(), Mode::Grpc); }
