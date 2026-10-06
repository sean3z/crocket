// A log message whose placeholders and arguments disagree must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

int main() { log::info("created user {user_id} on plan {plan}", 42); }
