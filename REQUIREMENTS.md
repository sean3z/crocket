# crocket

A C++ web framework in the same spirit as Rocket for Rust: ordinary functions as handlers, typed extractors as arguments, return value as the response, fairings and managed state on the application. It is **not** a port of Rocket.

Public API target is C++26 reflection and annotations. Compilers: GCC with C++26 reflection (experimental Clang as it lands). MSVC is out until `^^` ships. Internals may use C++20; that is not a second programming model and is not documented as an app API.

## Product goals

1. A handler is a function. `{id}` in the path is a parameter named `id`. Guards (`Json<T>`, `Auth`, `State<T>`) are more parameters. The return type is the HTTP response.
2. Route metadata lives on the function (`[[= http::get("/users/{id}")]]`). Mount a **namespace or controller**, not a handwritten list of functions.
3. Application-level `attach` (fairings) and `manage` (typed singletons). CORS, logging, metrics, and pools are not copied onto every handler.
4. Production behavior is built in: request ids, structured errors, route-template logs and metrics, deadlines, readiness, ignite checks, drain on shutdown.
5. One wire engine: **libwebsockets** (HTTP/1.1, HTTP/2, TLS). Handlers never include lws.

Non-goals: ORM, plugin ABI, websockets (unless lws makes a thin add-on cheap later), hot reload, scanning an entire translation unit for routes, reflection on the request path.

## Handler model

```cpp
namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  return std::format("Hello, {} year old named {}!", age, name);
}

[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth, State<Db> db)
    -> Result<Created<User>, ApiError>;

[[= http::get("/me")]]
auto me(Auth auth) -> Json<User>;

auto helper(int) -> int;  // no Route annotation → not mounted

}  // namespace api
```

Launch:

```cpp
int main() {
  crocket::build()
    .manage(DbPool::connect(cfg))
    .attach(Logger{})
    .attach(Cors::deny())
    .attach(Metrics{})
    .mount("/api", reflect_routes<^^api>())
    .launch({.host = "0.0.0.0", .port = 443,
             .tls_cert = "fullchain.pem", .tls_key = "privkey.pem"});
}
```

Rules:

- `{name}` binds to the parameter named `name`. Name or type mismatch is a **compile error**.
- Parameters not in the path are extractors. Unknown types do not compile.
- Return type implements `Responder`: `std::string`, `Json<T>`, status wrappers, `std::optional<T>` (empty → 404), `std::expected<T, ApiError>`.
- Handlers are non-template functions (C++26 `parameters_of` limit).
- One route annotation per function. Two verbs → two functions (or a verb-set annotation later).
- Discovery is a named namespace or class passed to `reflect_routes<^^…>()`. No whole-TU scan.
- Unannotated functions in that scope are ignored.
- A raw `Request&` / `Response&` hatch may exist for generated or edge code. It is not the documented happy path.

## Fairings and state

Fairing hooks: `on_ignite`, `on_request`, `on_response`, `on_shutdown`.  
`on_request` may finish the response and stop the chain.  
`on_response` runs in reverse attach order.  
Fairings stay short and synchronous (log, CORS, metrics). They do not parse JSON.

`manage(T)` stores one `T`. `State<T>` retrieves it. A mounted route that needs `State<T>` when `T` was not managed fails at **ignite**, not on the first request.

CORS is a fairing. Default deny. Not a per-route annotation.

## Errors

```cpp
struct ApiError {
  int status;
  std::string_view code;  // "auth.expired"
  std::string message;    // client-safe
  std::string detail;     // logs only
};
```

Extractor failures: `path.invalid` (404/422), `json.invalid` (422), `auth.expired` (401).  
Uncaught → 500 `internal`. Production responses never include `what()`.

## Production (framework, not application code)

- Request id from `X-Request-Id` or generated; present on logs, metrics, and error bodies.
- One completion log line: id, method, **route template**, status, code, duration, bytes.
- Metrics labeled by route template: count, duration, in-flight, extractor failures by kind.
- `GET /healthz` liveness; `GET /readyz` uses the same `State<T>` (e.g. pool checkout).
- Limits: header/body size, in-flight cap, pool wait → 503 `db.busy`.
- Deadline / cancel token available to blocking extractors.
- Ignite: cert/key load, required `State<T>`, conflicting verb+path.
- Shutdown: stop accept, drain with timeout, then drop pools.
- Debug-only `GET /__routes` (handler name, extractors, rank) behind a flag.

## Async

Sync functions are valid. `Task<T>` / awaitable returns are allowed on the same annotation. The router detects an awaitable result; there is no parallel `get_async` API. Fairings stay sync.

## Wire

libwebsockets only. FetchContent pins v4.3.5 with HTTP/2 and SSL. TLS cert/key on `LaunchOptions`. ALPN selects h1 vs h2 inside lws. Application code does not branch on protocol.

## Compiler and modules

- App code that uses annotations and `reflect_routes` requires C++26 reflection.
- Handlers live in a module or namespace the mount site can reflect.
- Modules themselves are not reflectable; reflect the exported namespace inside them.

## Acceptance

1. `hello(name, uint8_t age)` — unparsable age is 404 and the function does not run.
2. `create(Json<NewUser>, Auth, State<Db>)` — missing/invalid JSON is 422; missing auth is 401.
3. `me(Auth)` — no body extractor.
4. `helper` in `api` is not a route.
5. Path `{age}` with parameter `years` fails to compile, naming both identifiers.
6. Missing `.manage(Db)` while a route takes `State<Db>` fails ignite.
7. TLS launch serves HTTPS; HTTP/2 is available when the client ALPN-negotiates `h2`.
