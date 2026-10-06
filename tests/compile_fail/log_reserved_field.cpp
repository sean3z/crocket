// A placeholder named like a field crocket writes itself must not compile.
#include <crocket/crocket.hpp>
using namespace crocket;

int main() { log::info("handled {request_id}", 7); }
