// A gRPC service whose schema is its C++ declarations.
//
//   ./build/crocket_grpc --proto > greeter.proto     the .proto for other languages
//   ./build/crocket_grpc                             plaintext HTTP/2 on :50051
//   grpcurl -plaintext -proto greeter.proto -d '{"name": "Ada"}' \
//       localhost:50051 helloworld.Greeter/SayHello
//
// With CROCKET_TLS_CERT and CROCKET_TLS_KEY set it serves TLS instead (h2 via ALPN).

#include <crocket/crocket.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace crocket;

enum class Mood : std::uint8_t { calm, happy, grumpy };

struct HelloRequest {
  std::string name;                                    // field 1
  std::optional<std::string> title;                    // field 2
  [[= proto::field(4)]] std::vector<std::string> tags;  // field 4 (3 was retired)
  Mood mood = Mood::calm;                              // field 5
};

struct HelloReply {
  std::string message;
  std::uint64_t greeting_number = 0;
};

struct Counter {
  std::atomic<std::uint64_t> n{0};
  Counter() = default;
  Counter(Counter&& o) noexcept : n(o.n.load()) {}
};

namespace greeter {

[[= grpc::rpc]]
auto say_hello(HelloRequest req, State<Counter> counter) -> Result<HelloReply> {
  if (req.name.empty())
    return std::unexpected(ApiError::bad_request("hello.name", "name is required"));  // INVALID_ARGUMENT
  std::string who = req.title ? *req.title + " " + req.name : req.name;
  std::string message = (req.mood == Mood::grumpy ? "Oh. Hello, " : "Hello, ") + who + "!";
  for (auto& t : req.tags) message += " #" + t;
  return HelloReply{message, ++counter->n};
}

[[= grpc::rpc]]
auto whoami(Auth auth) -> HelloReply { return {auth.subject}; }  // UNAUTHENTICATED without a token

/// Custom metadata arrives as request headers (grpcurl -H 'x-tenant: acme').
[[= grpc::rpc]]
auto tenant(Header<"x-tenant", std::optional<std::string>> tenant) -> HelloReply {
  return {tenant->value_or("(none)")};
}

}  // namespace greeter

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--proto") {
    std::fputs(grpc::proto_file<^^greeter>("helloworld").c_str(), stdout);
    return 0;
  }
  auto env = [](const char* name) { return std::string(std::getenv(name) ? std::getenv(name) : ""); };
  LaunchOptions opts{.port = 50051, .tls_cert = env("CROCKET_TLS_CERT"), .tls_key = env("CROCKET_TLS_KEY")};
  if (auto port = env("CROCKET_PORT"); !port.empty()) opts.port = static_cast<std::uint16_t>(std::stoi(port));
  if (auto loops = env("CROCKET_EVENT_LOOPS"); !loops.empty()) opts.event_loops = unsigned(std::stoi(loops));
  opts.h2_prior_knowledge = opts.tls_cert.empty();  // gRPC's insecure channels speak h2 without TLS
  return build(Config::from_env())
      .attach(Logger{})
      .manage(Counter{})
      .manage(Authenticator{[](std::string_view token) -> std::expected<Auth, ApiError> {
        if (token == "letmein") return Auth{"ada"};
        return std::unexpected(ApiError::unauthorized("auth.invalid", "unknown token"));
      }})
      .mount("helloworld", reflect_routes<^^greeter>(), Mode::Grpc)
      .launch(opts);
}
