// A fuller service: managed pool + authenticator, fairings, TLS, readiness,
// JSON bodies, deadlines, and a controller.
//
//   ./build/crocket_serve                                 # http://0.0.0.0:8000
//   CROCKET_PORT=8443 CROCKET_TLS_CERT=cert.pem CROCKET_TLS_KEY=key.pem ./build/crocket_serve
//   CROCKET_DEBUG_ROUTES=1 ./build/crocket_serve          # exposes GET /__routes
//
//   curl localhost:8000/api/users/1
//   curl -X POST localhost:8000/api/users -H 'authorization: Bearer alice' \
//        -H 'content-type: application/json' -d '{"email":"a@example.com"}'
//   curl localhost:8000/metrics

#include <crocket/crocket.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace crocket;
using namespace std::chrono_literals;

// ---- domain -------------------------------------------------------------------------

struct NewUser {
  std::string email;
  std::optional<std::string> name;
};

struct User {
  std::uint64_t id;
  std::string email;
  std::optional<std::string> name;
  std::string created_by;
};

/// Pretend database: a shared in-memory table behind a pool of "connections".
struct Table {
  std::mutex mu;
  std::map<std::uint64_t, User> rows;
  std::uint64_t next = 1;
};

struct Conn {
  std::shared_ptr<Table> table;
  auto insert(NewUser u, std::string by) -> User {
    std::lock_guard lk(table->mu);
    User row{table->next++, std::move(u.email), std::move(u.name), std::move(by)};
    table->rows.emplace(row.id, row);
    return row;
  }
  auto find(std::uint64_t id) -> std::optional<User> {
    std::lock_guard lk(table->mu);
    auto it = table->rows.find(id);
    if (it == table->rows.end()) return std::nullopt;
    return it->second;
  }
  auto all() -> std::vector<User> {
    std::lock_guard lk(table->mu);
    std::vector<User> out;
    for (auto& [id, u] : table->rows) out.push_back(u);
    return out;
  }
};

using DbPool = Pool<Conn>;

// ---- handlers -----------------------------------------------------------------------

namespace api {

[[= http::get("/users/{id}")]]
auto get_user(std::uint64_t id, State<DbPool> db, Deadline d) -> Result<std::optional<Json<User>>, ApiError> {
  auto conn = db->checkout(d);  // 503 db.busy if no connection before the deadline
  if (!conn) return std::unexpected(conn.error());
  auto u = (*conn)->find(id);
  if (!u) return std::optional<Json<User>>{};  // 404
  return Json<User>{*u};
}

[[= http::get("/users")]]
auto list_users(State<DbPool> db, Deadline d) -> Result<Json<std::vector<User>>, ApiError> {
  auto conn = db->checkout(d);
  if (!conn) return std::unexpected(conn.error());
  return Json<std::vector<User>>{(*conn)->all()};
}

[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth, State<DbPool> db, Deadline d) -> Result<Created<User>, ApiError> {
  if (body->email.find('@') == std::string::npos)
    return std::unexpected(ApiError::unprocessable("user.email", "email must contain '@'"));
  auto conn = db->checkout(d);
  if (!conn) return std::unexpected(conn.error());
  User u = (*conn)->insert(std::move(*body), auth.subject);
  return Created<User>{u, "/api/users/" + std::to_string(u.id)};
}

[[= http::get("/me")]]
auto me(Auth auth) -> Json<Auth> { return Json<Auth>{auth}; }

/// Sleeps in small steps, honouring the request deadline and client cancellation.
[[= http::get("/slow/{ms}")]]
auto slow(std::uint32_t ms, Deadline d) -> Result<std::string, ApiError> {
  auto until = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < until) {
    if (d.expired()) return std::unexpected(ApiError{504, "deadline.exceeded", "gave up", "slow handler cancelled"});
    std::this_thread::sleep_for(10ms);
  }
  return std::format("slept {} ms", ms);
}

/// Async handler: same annotation, awaitable result.
[[= http::get("/square/{n}")]]
auto square(std::int64_t n) -> Task<Json<std::int64_t>> { co_return Json<std::int64_t>{n * n}; }

}  // namespace api

/// Controller: a managed instance with its own configuration.
class Info {
 public:
  explicit Info(std::string version) : version_(std::move(version)) {}

  [[= http::get("/version")]]
  auto version() const -> std::string { return version_; }

 private:
  std::string version_;
};

// ---- wiring -------------------------------------------------------------------------

static std::string env(const char* name, std::string fallback = {}) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : fallback;
}

int main() {
  auto table = std::make_shared<Table>();
  DbPool pool(8, [&] { return Conn{table}; }, {.max_wait = 2s});

  Authenticator auth{[](std::string_view token) -> std::expected<Auth, ApiError> {
    // Demo tokens. A real verifier checks a signature and expiry.
    if (token == "alice") return Auth{"alice", {"users:write"}};
    if (token == "expired") return std::unexpected(ApiError::unauthorized("auth.expired", "token expired"));
    return std::unexpected(ApiError::unauthorized("auth.invalid", "unknown token"));
  }};

  Config cfg;
  cfg.request_timeout = 10s;
  cfg.drain_timeout = 5s;
  cfg.debug_routes = !env("CROCKET_DEBUG_ROUTES").empty();

  ListenOptions listen;
  listen.host = env("CROCKET_HOST", "0.0.0.0");
  listen.port = static_cast<std::uint16_t>(std::stoi(env("CROCKET_PORT", "8000")));
  listen.tls_cert = env("CROCKET_TLS_CERT");
  listen.tls_key = env("CROCKET_TLS_KEY");

  return App{cfg}
      .manage(std::move(pool))  // Pool::ready() also drives GET /readyz
      .manage(std::move(auth))
      .manage(Info{"crocket-serve 0.1.0"})
      .attach(Logger{})
      .attach(Cors::allow_origins({"https://app.example.com"}))
      .attach(Metrics{})
      .mount("/api", reflect_routes<^^api>())
      .mount("/", reflect_routes<^^Info>())
      .listen(listen);
}
