# crocket: design and principles

crocket is a C++ web framework in the spirit of Rocket for Rust. Handlers are ordinary
functions, typed extractors are their arguments, and the return value is the response.
Fairings and managed state belong to the application. It is **not** a port of Rocket.

This page states what crocket is for, what it will not do, and the rules that decide
whether a feature belongs. The [README](../README.md) shows how to use it, and
[ENGINE.md](ENGINE.md) explains how the wire engine works.

## Goals

1. **A handler is a function.** `{id}` in the path is a parameter named `id`. Guards such
   as `Json<T>`, `Auth` and `State<T>` are more parameters. The return type is the HTTP
   response.
2. **Route metadata lives on the function** (`[[= http::get("/users/{id}")]]`). You mount
   a namespace or a controller class, not a handwritten list of functions.
3. **Cross-cutting behaviour belongs to the application.** `attach` adds fairings and
   `manage` adds typed singletons. CORS, logging, metrics and pools are not repeated on
   every handler.
4. **Production behaviour is built in**, not left to each application: request ids,
   structured errors, logs and metrics by route template, deadlines, readiness, limits,
   ignite checks, and draining on shutdown.
5. **One wire engine:** h2o, for HTTP/1.1, HTTP/2 and TLS. Handlers never include h2o.

## Non-goals

- An ORM, or a plugin ABI.
- Reloading application code without a restart. (Reloading the TLS certificate is
  supported; that is configuration, not code.)
- Discovering routes by scanning a whole translation unit.
- Reflection, string lookups or type erasure per argument on the request path.
- A second wire engine, or a second programming model for async code.

WebSockets, server-sent events and streamed bodies are not built yet. When they are,
they will be engine features behind the same handler model, not a separate API.

## Principles

These decide what goes in, and how:

- **The signature is the configuration.** Which state a route needs, whether it reads
  the body, how its errors are counted and how it is described in OpenAPI all come from
  its parameter and return types. Nothing is registered twice.
- **Mistakes fail as early as possible:** at compile time when the types say so (a
  capture without a matching parameter, an unknown extractor), at ignite when only the
  assembled application can tell (unmanaged state, conflicting routes, a bad
  certificate), and never on the first request.
- **Expensive features are opt-in.** `Logger`, `Metrics`, `OpenApi` and `Cors` cost
  nothing until attached. What is on by default must be cheap and safe: `Shield`'s
  security headers, request ids, and ETags for bodies up to `Config::etag_max_bytes`.
- **Every resource a client can make the server hold has a bound:** connections,
  HTTP/2 streams, header and body sizes, bodies held in memory, requests in flight, and
  time. The defaults follow h2o and Rocket where they have one (see the
  [limits](../README.md#limits)).
- **Dead work is dropped.** A request whose deadline has passed, or whose client has
  gone, does not run its handler. A handler that ignores its deadline is visible in
  metrics and logs.
- **Tests see what production sees.** `LocalClient` normalizes and routes paths exactly
  as the server does, so an in-process test cannot pass on an input the server treats
  differently.
- **Untrusted input is fuzzed.** Every parser that reads bytes from the network has a
  fuzz target (see [Fuzzing](../README.md#fuzzing)).
- **Performance is measured, not assumed.** `./dev bench` compares crocket with Rocket
  and with h2o alone; a change that costs CPU per request has to say why.

## Handler model

```cpp
namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  return std::format("Hello, {} year old named {}!", age, name);
}

[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth, State<Db> db) -> Result<Created<User>>;

[[= http::get("/me")]]
auto me(Auth auth) -> Json<User>;

auto helper(int) -> int;  // no route annotation: not mounted

}  // namespace api
```

Launch:

```cpp
int main() {
  return crocket::build()
      .manage(DbPool::connect(cfg))
      .attach(Logger{})
      .attach(Cors::allow_origins({"https://app.example.com"}))
      .attach(Metrics{})
      .mount("/api", reflect_routes<^^api>())
      .launch({.port = 443, .tls_cert = "fullchain.pem", .tls_key = "privkey.pem"});
}
```

Rules:

- `{name}` binds to the parameter named `name`. A name or type mismatch is a compile
  error that names both identifiers.
- Parameters not in the path are extractors. An unknown type does not compile.
- The return type is a `Responder`: `std::string`, `Json<T>`, status wrappers
  (`Created<T>`, `NoContent`), `std::optional<T>` (empty is 404), `Result<T>`
  (`std::expected<T, ApiError>`), `Cacheable<T>`, or an awaitable of one (`Task<T>`).
- Handlers are not templates: reflection needs concrete parameter types.
- One route annotation per function. Two verbs are two functions.
- Routes are discovered from a named namespace or class passed to
  `reflect_routes<^^…>()`. Unannotated functions in it are ignored.
- `const Request&` and `Response&` parameters exist for edge cases. They are not the
  documented happy path.
- A path is split on `/` before it is decoded, so a capture can hold any character, and
  dot segments are removed before routing.

## Fairings and state

- Fairing hooks are `on_ignite`, `on_request`, `on_response` and `on_shutdown`.
- `on_request` may answer the request itself and stop the chain.
- `on_response` runs in reverse attach order.
- Fairings are short and synchronous (logging, CORS, metrics). They do not parse request
  bodies.
- `manage(T)` stores one `T`, and `State<T>` retrieves it. A mounted route that needs
  `State<T>` when no `T` is managed fails at ignite.
- `Shield` (security headers) is attached by default. `Shield::none()` turns it off.
- CORS is a fairing, not a per-route annotation. Without `Cors`, no cross-origin headers
  are sent, so browsers refuse cross-origin calls.

## Errors

```cpp
struct ApiError {
  int status;
  std::string_view code;         // stable and dotted: "auth.expired"
  std::string message;           // safe for clients
  std::string detail;            // logs only (shown in error bodies in the dev profile)
  std::vector<FieldError> errors;  // per-field failures, e.g. from validation
};
```

- Error bodies are RFC 9457 problem details (`application/problem+json`): `title`,
  `status`, `detail` (the message), `code`, `request_id`, and `errors` when present.
- Extractor failures have their own codes: `path.invalid` (404), `query.invalid` (422),
  `header.missing` and `header.invalid` (400), `json.unsupported_media_type` (415),
  `json.invalid`, `json.validation`, `json.unknown_field` and others (422), and
  `auth.missing`, `auth.invalid` or the verifier's own code such as `auth.expired` (401).
- An uncaught exception is 500 `internal`. Its `what()` never reaches the client.

## Production baseline

What every crocket application gets without writing it:

- A request id, from a valid `X-Request-Id` or generated, on every response, log line
  and error body.
- One completion log line per request (with `Logger`): id, method, route template,
  status, code, duration and sizes.
- Metrics by route template (with `Metrics`): counts, durations, in-flight requests,
  extractor failures, workers, connections, and requests dropped unrun.
- `GET /healthz` for liveness, and `GET /readyz` driven by managed state such as a pool's
  ability to check out a connection.
- Limits on connections, HTTP/2 streams, headers, bodies, bodies in memory, requests in
  flight, and pool waits (503 `db.busy`).
- A deadline and stop token on every request, honoured by extractors and pools.
- Ignite checks: the certificate and key load, required state is managed, and no two
  routes conflict.
- Graceful shutdown: stop accepting, drain within a timeout, then destroy managed state.
- Certificate rotation on SIGHUP or when the files change, without dropping connections.
- `GET /__routes`, listing handlers, extractors and ranks, behind a flag.

## Async

Plain functions and `Task<T>` handlers use the same annotations: the router detects an
awaitable result, and there is no parallel `get_async` API. `Task<T>` handlers run on
the event loops. Plain functions run on a worker pool, or on the event loops once they
have proved fast (adaptive placement). Fairings stay synchronous.

## Wire

h2o only, as the static library `libh2o-evloop`. FetchContent pins a commit, which
`./dev h2o-update` moves to the newest h2o. The certificate and key are
`Config::server`. ALPN chooses HTTP/1.1 or HTTP/2 inside h2o, and plain-text HTTP/2
(prior knowledge) is available for gRPC. Application code never branches on protocol.

## Compiler and modules

- Application code that uses annotations and `reflect_routes` needs C++26 reflection.
  crocket is developed on GCC 16; Clang follows as its reflection support lands. MSVC
  is out until it ships `^^`.
- Handlers live in a namespace or class the mount site can reflect.
- Modules themselves are not reflectable: reflect the exported namespace inside them.

## Acceptance criteria

The test suite checks each of these:

1. `hello(name, uint8_t age)`: an age that does not parse is 404, and the function does
   not run.
2. `create(Json<NewUser>, Auth, State<Db>)`: missing or invalid JSON is 422, and a
   missing token is 401.
3. `me(Auth)`: a route with no body extractor.
4. `helper` in `api` is not a route.
5. A path with `{age}` and a parameter named `years` does not compile, and the error
   names both identifiers.
6. A route taking `State<Db>` without `.manage(Db)` fails ignite.
7. A TLS launch serves HTTPS, with HTTP/2 when the client negotiates `h2` over ALPN.
