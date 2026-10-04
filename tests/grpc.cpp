// gRPC and protobuf: the wire encoding byte for byte, decode errors, unary
// calls through LocalClient (same pipeline as the engine), the generated
// .proto, and mount validation. tests/grpc_h2.sh covers the real socket.

#include <crocket/crocket.hpp>

#include <cstdio>
#include <format>
#include <string>

using namespace crocket;

// ---- tiny harness --------------------------------------------------------------
static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) {                                                                    \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    ++g_checks;                                                                       \
    auto va_ = (a);                                                                   \
    auto vb_ = (b);                                                                   \
    if (!(va_ == vb_)) {                                                              \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (got %s)\n", __FILE__, __LINE__,  \
                   #a, #b, std::format("{}", va_).c_str());                           \
    }                                                                                 \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }
static bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != hay.npos; }

static std::string hex(std::string_view bytes) {
  std::string out;
  for (unsigned char c : bytes) out += std::format("{:02x}", c);
  return out;
}

// ---- messages ---------------------------------------------------------------------

enum class Mood : std::uint8_t { calm, happy, grumpy };
enum class Tier { gold = 1, silver = 2 };  // no zero: the .proto gains TIER_UNSPECIFIED

struct Address {
  std::string city;
  std::uint32_t zip = 0;
};

struct HelloRequest {
  std::string name;                                        // 1
  std::int32_t age = 0;                                    // 2
  [[= proto::field(5)]] std::optional<std::string> nickname;  // 5
  std::vector<std::int64_t> lucky;                         // 6
  Mood mood = Mood::calm;                                  // 7
  std::map<std::string, std::int32_t> scores;              // 8
  std::vector<Address> addresses;                          // 9
  proto::Bytes avatar;                                     // 10
  double ratio = 0;                                        // 11
  bool admin = false;                                      // 12
  std::optional<Tier> tier;                                // 13
  std::uint8_t level = 0;                                  // 14
};

struct HelloReply {
  std::string message;
};

struct Remaining {
  std::int64_t ms = 0;
};

namespace greeter {

[[= grpc::rpc]]
auto say_hello(HelloRequest req) -> HelloReply {
  std::string m = "Hello, " + req.name;
  if (req.nickname) m += " aka " + *req.nickname;
  if (!req.addresses.empty()) m += " from " + req.addresses.back().city;
  return {m};
}

[[= grpc::rpc]]
auto whoami(Auth auth) -> HelloReply { return {auth.subject}; }

[[= grpc::rpc]]
auto find(HelloRequest req) -> std::optional<HelloReply> {
  if (req.name.empty()) return std::nullopt;
  return HelloReply{"found " + req.name};
}

[[= grpc::rpc]]
auto create(HelloRequest req) -> Result<HelloReply> {
  if (req.name == "taken") return std::unexpected(grpc::error(grpc::Code::AlreadyExists, "user.exists", "name taken"));
  if (req.name == "bad") return std::unexpected(ApiError::unprocessable("user.name", "bad name"));
  return HelloReply{"created " + req.name};
}

[[= grpc::rpc]]
auto ping() -> void {}

[[= grpc::rpc]]
auto boom(HelloRequest req) -> HelloReply { (void)req;
  throw std::runtime_error("secret");
}

[[= grpc::rpc]]
auto remaining(Deadline d) -> Task<Remaining> {
  co_return Remaining{std::chrono::duration_cast<std::chrono::milliseconds>(d.remaining()).count()};
}

}  // namespace greeter

namespace web {
[[= http::get("/web")]]
auto page() -> std::string { return "web"; }
}  // namespace web

static Authenticator test_auth() {
  return Authenticator{[](std::string_view token) -> std::expected<Auth, ApiError> {
    if (token == "alice") return Auth{"alice"};
    return std::unexpected(ApiError::unauthorized("auth.invalid", "unknown token"));
  }};
}

// ---- tests -----------------------------------------------------------------------

int main() {
  section("proto: wire bytes match protobuf's encoding");
  {
    // name = "hi" (field 1, LEN), age = -1 (field 2, varint sign-extended to 10 bytes)
    CHECK_EQ(hex(proto::encode(HelloRequest{.name = "hi", .age = -1})),
             std::string("0a02686910ffffffffffffffffff01"));
    // Defaults are not sent (proto3 implicit presence); an empty message is empty.
    CHECK_EQ(proto::encode(HelloRequest{}).size(), std::size_t(0));
    // Pinned field 5, then 6 packed: 1, 300.
    CHECK_EQ(hex(proto::encode(HelloRequest{.nickname = "", .lucky = {1, 300}})), std::string("2a0032030" "1ac02"));
    // Enum, map entry {1: "a", 2: 7}, nested message, bytes, double, bool.
    HelloRequest r{.mood = Mood::grumpy, .scores = {{"a", 7}}, .addresses = {{"Oslo", 150}}};
    CHECK_EQ(hex(proto::encode(r)), std::string("3802" "42050a01611007" "4a090a044f736c6f109601"));
    CHECK_EQ(hex(proto::encode(HelloRequest{.avatar = {std::byte{1}, std::byte{255}}, .ratio = 1.5, .admin = true})),
             std::string("520201ff" "59000000000000f83f" "6001"));
  }

  section("proto: round trip and tolerant decoding");
  {
    HelloRequest in{.name = "Zoë",
                    .age = -42,
                    .nickname = "z",
                    .lucky = {-1, 0, 1LL << 40},
                    .mood = Mood::happy,
                    .scores = {{"x", -3}, {"y", 0}},
                    .addresses = {{"Oslo", 150}, {"Rome", 0}},
                    .avatar = {std::byte{0}, std::byte{9}},
                    .ratio = -0.0,
                    .admin = true,
                    .tier = Tier::gold,
                    .level = 200};
    auto out = proto::decode<HelloRequest>(proto::encode(in));
    CHECK(out.has_value());
    if (out) {
      CHECK_EQ(out->name, in.name);
      CHECK_EQ(out->age, -42);
      CHECK_EQ(out->nickname.value_or("?"), std::string("z"));
      CHECK(out->lucky == in.lucky);
      CHECK(out->mood == Mood::happy);
      CHECK(out->scores == in.scores);
      CHECK_EQ(out->addresses.size(), std::size_t(2));
      CHECK_EQ(out->addresses[0].zip, 150u);
      CHECK(out->avatar == in.avatar);
      CHECK(std::signbit(out->ratio));  // -0.0 is not the default, so it is sent
      CHECK(out->admin);
      CHECK(out->tier == Tier::gold);
      CHECK_EQ(int(out->level), 200);
    }
    // An unknown field (99, varint) is skipped; unpacked repeated numbers are accepted.
    std::string extra = proto::encode(HelloRequest{.name = "a"}) + "\x98\x06\x05" + "\x30\x07\x30\x08";
    auto tolerant = proto::decode<HelloRequest>(extra);
    CHECK(tolerant && tolerant->name == "a" && tolerant->lucky == std::vector<std::int64_t>({7, 8}));
  }

  section("proto: decode errors name the field");
  {
    auto wrong_wire = proto::decode<HelloRequest>(std::string("\x08\x01", 2));  // name as varint
    CHECK(!wrong_wire);
    CHECK_EQ(wrong_wire.error(), std::string("field 'name': expected string, got wire type 0"));
    auto bad_utf8 = proto::decode<HelloRequest>(std::string("\x0a\x02\xc3\x28", 4));
    CHECK_EQ(bad_utf8.error(), std::string("field 'name': string is not valid UTF-8"));
    auto nested = proto::decode<HelloRequest>(std::string("\x4a\x02\x10\xff", 4));  // addresses[].zip truncated
    CHECK_EQ(nested.error(), std::string("field 'addresses.zip': truncated varint"));
    auto range = proto::decode<HelloRequest>(std::string("\x70\xac\x02", 3));  // level = 300 into uint8_t
    CHECK_EQ(range.error(), std::string("field 'level': value 300 out of range"));
    auto overrun = proto::decode<HelloRequest>(std::string("\x0a\x05hi", 4));
    CHECK(contains(overrun.error(), "runs past the end"));
  }

  Crocket app;
  app.manage(test_auth()).mount("helloworld", reflect_routes<^^greeter>(), Mode::Grpc).mount("/", reflect_routes<^^web>());
  LocalClient client(app);

  section("grpc: unary call");
  {
    HelloRequest req{.name = "Ada", .nickname = "Countess", .addresses = {{"London", 0}}};
    auto r = client.grpc("/helloworld.Greeter/SayHello", proto::encode(req)).dispatch();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers.get("content-type").value_or(""), std::string_view("application/grpc"));
    CHECK_EQ(r.trailers.get("grpc-status").value_or(""), std::string_view("0"));
    auto reply = grpc::read_reply<HelloReply>(r);
    CHECK(reply.has_value());
    CHECK_EQ(reply.value_or(HelloReply{"?"}).message, std::string("Hello, Ada aka Countess from London"));
    // HTTP routes still work beside gRPC.
    CHECK_EQ(client.get("/web").dispatch().body, std::string("web"));
  }

  section("grpc: empty request and reply, extractors, tasks");
  {
    auto ping = client.grpc("/helloworld.Greeter/Ping", "").dispatch();
    CHECK(grpc::status_of(ping).ok());
    CHECK_EQ(hex(ping.body), std::string("0000000000"));  // one empty message
    auto who = client.grpc("/helloworld.Greeter/Whoami", "").bearer("alice").dispatch();
    CHECK_EQ(grpc::read_reply<HelloReply>(who).value_or(HelloReply{"?"}).message, std::string("alice"));
    auto anon = client.grpc("/helloworld.Greeter/Whoami", "").dispatch();
    CHECK(grpc::status_of(anon).code == grpc::Code::Unauthenticated);
    CHECK_EQ(grpc::status_of(anon).message, std::string("missing bearer token"));
    CHECK_EQ(anon.status, 401);  // the HTTP equivalent, for logs and metrics
    CHECK(anon.trailers.empty());  // trailers-only: the status travels in the headers
    CHECK_EQ(anon.headers.get("crocket-error-code").value_or(""), std::string_view("auth.missing"));
    CHECK_EQ(anon.failure_kind, std::string_view("auth"));
  }

  section("grpc: errors map to status codes");
  {
    auto code = [&](std::string_view method, const HelloRequest& req) {
      return grpc::status_of(client.grpc(std::string("/helloworld.Greeter/") + std::string(method), proto::encode(req))
                                 .dispatch());
    };
    CHECK(code("Find", {.name = ""}).code == grpc::Code::NotFound);
    CHECK(code("Find", {.name = "x"}).ok());
    auto taken = code("Create", {.name = "taken"});
    CHECK(taken.code == grpc::Code::AlreadyExists);
    CHECK_EQ(taken.message, std::string("name taken"));
    CHECK(code("Create", {.name = "bad"}).code == grpc::Code::InvalidArgument);  // 422
    auto boom = code("Boom", {});
    CHECK(boom.code == grpc::Code::Internal);
    CHECK(!contains(boom.message, "secret"));
    auto missing = grpc::status_of(client.grpc("/helloworld.Greeter/Nope", "").dispatch());
    CHECK(missing.code == grpc::Code::Unimplemented);
    CHECK(contains(missing.message, "/helloworld.Greeter/Nope"));
    auto bad_proto = client.grpc("/helloworld.Greeter/SayHello", std::string("\x08\x01", 2)).dispatch();
    CHECK(grpc::status_of(bad_proto).code == grpc::Code::InvalidArgument);
    CHECK_EQ(grpc::status_of(bad_proto).message, std::string("field 'name': expected string, got wire type 0"));
    CHECK_EQ(bad_proto.failure_kind, std::string_view("proto"));
    auto compressed = client.post("/helloworld.Greeter/Ping").header("content-type", "application/grpc")
                          .body(std::string("\x01\x00\x00\x00\x00", 5)).dispatch();
    CHECK(grpc::status_of(compressed).code == grpc::Code::Unimplemented);
    auto two = client.post("/helloworld.Greeter/Ping").header("content-type", "application/grpc+proto")
                   .body(grpc::frame("") + grpc::frame("")).dispatch();
    CHECK(grpc::status_of(two).code == grpc::Code::InvalidArgument);
    // Not gRPC at all: plain HTTP 415 with the JSON error body.
    auto json = client.post("/helloworld.Greeter/Ping").json("{}").dispatch();
    CHECK_EQ(json.status, 415);
    CHECK(contains(json.body, "grpc.content_type"));
  }

  section("grpc: grpc-timeout shortens the deadline");
  {
    auto r = client.grpc("/helloworld.Greeter/Remaining", "").header("grpc-timeout", "200m").dispatch();
    auto ms = grpc::read_reply<Remaining>(r).value_or(Remaining{-1}).ms;
    CHECK(ms > 0 && ms <= 200);
    auto r2 = client.grpc("/helloworld.Greeter/Remaining", "").header("grpc-timeout", "99H").dispatch();
    CHECK(grpc::read_reply<Remaining>(r2).value_or(Remaining{-1}).ms > 1000);  // the configured 30 s
    CHECK(grpc::read_reply<Remaining>(r2).value_or(Remaining{-1}).ms <= 30'000);
  }

  section("grpc: dev profile adds the detail to grpc-message");
  {
    Crocket dev_app{Config::dev()};
    dev_app.mount("", reflect_routes<^^greeter>(), Mode::Grpc).manage(test_auth());
    LocalClient dev(dev_app);
    auto r = dev.grpc("/Greeter/Boom", "").dispatch();  // no package
    CHECK(contains(grpc::status_of(r).message, "uncaught exception in greeter::boom: secret"));
  }

  section("grpc: mount validation");
  {
    auto problems = [](Crocket&& a) {
      auto r = a.ignite();
      return r ? std::string() : r.error().message();
    };
    CHECK(contains(problems(Crocket{}.mount("/", reflect_routes<^^greeter>())),
                   "greeter::say_hello is a [[= grpc::rpc]] method; mount its scope with Mode::Grpc"));
    CHECK(contains(problems(Crocket{}.mount("api", reflect_routes<^^web>(), Mode::Grpc)),
                   "GET /web (web::page) is an HTTP route mounted with Mode::Grpc"));
    CHECK(contains(problems(Crocket{}.mount("/api", reflect_routes<^^greeter>(), Mode::Grpc)),
                   "is a protobuf package"));
    CHECK(contains(problems(Crocket{}.mount("a..b", reflect_routes<^^greeter>(), Mode::Grpc)),
                   "is a protobuf package"));
    CHECK(contains(problems(Crocket{}
                                .mount("x", reflect_routes<^^greeter>(), Mode::Grpc)
                                .mount("x", reflect_routes<^^greeter>(), Mode::Grpc)),
                   "route conflict"));
    CHECK(problems(Crocket{}.manage(test_auth()).mount("acme.v1", reflect_routes<^^greeter>(), Mode::Grpc)).empty());
  }

  section("grpc: the .proto comes from the C++ declarations");
  {
    auto text = grpc::proto_file<^^greeter>("helloworld");
    for (auto line : {"syntax = \"proto3\";", "package helloworld;", "import \"google/protobuf/empty.proto\";",
                      "service Greeter {", "  rpc SayHello (HelloRequest) returns (HelloReply);",
                      "  rpc Whoami (google.protobuf.Empty) returns (HelloReply);",
                      "  rpc Ping (google.protobuf.Empty) returns (google.protobuf.Empty);",
                      "  rpc Remaining (google.protobuf.Empty) returns (Remaining);", "message HelloRequest {",
                      "  string name = 1;", "  int32 age = 2;", "  optional string nickname = 5;",
                      "  repeated int64 lucky = 6;", "  Mood mood = 7;", "  map<string, int32> scores = 8;",
                      "  repeated Address addresses = 9;", "  bytes avatar = 10;", "  double ratio = 11;",
                      "  bool admin = 12;", "  optional Tier tier = 13;", "  uint32 level = 14;",
                      "message Address {", "  uint32 zip = 2;", "enum Mood {", "  MOOD_CALM = 0;", "  MOOD_GRUMPY = 2;",
                      "enum Tier {\n  TIER_UNSPECIFIED = 0;\n  TIER_GOLD = 1;"})
      CHECK(contains(text, line));
    CHECK(!contains(grpc::proto_file<^^greeter>(""), "package"));
  }

  std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
