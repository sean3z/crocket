// A validation annotation on a member it cannot apply to must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Signup {
  [[= json::min(1)]] std::string name;
};

int main() { return int(json::validate(Signup{}).size()); }
