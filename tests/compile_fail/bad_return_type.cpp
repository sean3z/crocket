// A return type that is not a Responder (or awaitable of one) must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct NotAResponse { int* p; };
namespace bad {
[[= http::get("/r")]]
auto get() -> NotAResponse { return {}; }
}  // namespace bad

int main() { App{}.mount("/", reflect_routes<^^bad>()); }
