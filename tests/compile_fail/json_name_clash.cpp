// Two members that map to the same JSON key must not compile: which one a
// document's value would land in is ambiguous.
#include <crocket/crocket.hpp>
using namespace crocket;

struct [[= json::rename_all(json::camel_case)]] Account {
  std::string user_id;
  [[= json::rename("userId")]] std::string legacy_id;
};

int main() { return int(json::to_string(Account{}).size()); }
