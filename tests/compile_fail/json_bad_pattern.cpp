// A malformed json::pattern must not compile: the regex is compiled with the program.
#include <crocket/crocket.hpp>
using namespace crocket;

struct Sku {
  [[= json::pattern("^[A-Z]{3-}$")]] std::string code;
};

int main() { return int(json::validate(Sku{}).size()); }
