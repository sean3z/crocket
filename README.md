# crocket

A C++ web framework in the same spirit as [Rocket](https://rocket.rs) for Rust. Not a port.

Handlers are functions. Path captures and extractors are arguments. The return value is the response. Routes are declared with C++26 annotations and discovered with reflection. The wire layer is libwebsockets (HTTP/1.1, HTTP/2, TLS).

```cpp
namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string;

}  // namespace api

App{}
  .attach(Logger{})
  .mount("/", reflect_routes<^^api>())
  .listen({.host = "0.0.0.0", .port = 8000});
```

## Why reflection?

Most C++ web frameworks bind routes with runtime registration (`app.get("/x", lambda)`), macros or a code generator. crocket uses C++26 static reflection instead. Routes are ordinary functions: the annotation is a normal C++ value, and `reflect_routes<^^api>()` reads the functions, their annotations and their parameters at compile time. That buys a few things:

- **Nothing to register or keep in sync.** Mounting a namespace or class picks up every annotated function in it; you can't forget to add a route to a list. Unannotated helpers are ignored.
- **Mistakes are compile errors, in your words.** Reflection can see parameter *names*, which templates alone cannot. A path capture is matched to its parameter by name, so a mismatch fails the build with a message naming both identifiers ("did you mean parameter 'years'?"). The same applies to a type that can't be parsed from a path, an unknown extractor, or a return type that isn't a responder.
- **No runtime cost.** Reflection runs only in the compiler. Each route becomes a plain function pointer to a generated invoker, with no string lookups, RTTI or type erasure per argument on the request path.
- **The signature is the configuration.** Which state a route needs, whether it reads the body, and how its errors are counted are all derived from the parameter types. That is how a missing `.manage(...)` is caught at ignite rather than on the first request.
- **One mechanism, no boilerplate.** The same reflection maps JSON bodies and query strings onto your structs, with no derive macros or field lists.
- **Handlers stay testable.** Because they are plain functions, you can call them directly in unit tests, step into them in a debugger, and navigate to them in an IDE.

The trade-off is toolchain reach and build time. You need a compiler with C++26 reflection (developed on GCC 16), and heavily annotated translation units compile more slowly than hand-registered routes.

See [REQUIREMENTS.md](REQUIREMENTS.md) and [docs/ENGINE.md](docs/ENGINE.md).

## Using crocket

Add crocket to a CMake project with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(crocket
  GIT_REPOSITORY https://github.com/sean3z/crocket.git
  GIT_TAG main)   # pin a release tag or commit once there is one
FetchContent_MakeAvailable(crocket)

target_link_libraries(my_app PRIVATE crocket::crocket)
```

For a vendored copy or a git submodule, use
`FetchContent_Declare(crocket SOURCE_DIR path/to/crocket)` instead.

Linking `crocket::crocket` adds `-std=c++26 -freflection` to your target. crocket's
own examples and tests are not built, and libwebsockets stays private: it is not
linked into your headers or your CMake cache.

You need:

- **GCC 16.2 or later.** Ubuntu 26.04's `g++-16` package is too old (see
  [Building](#building)). Clang is not tested yet.
- **CMake 3.28 or later**, and Ninja or Make.
- **OpenSSL development headers** (`libssl-dev`), because libwebsockets is built with TLS.
- **Network access on the first configure**, to fetch libwebsockets v4.3.5 from GitHub.
  Set `FETCHCONTENT_SOURCE_DIR_LIBWEBSOCKETS` to a local checkout to build offline.

There are no `install()` rules yet, so `find_package(crocket)` does not work.

## Rocket, in C++

If you know [Rocket](https://rocket.rs), almost every concept carries over. The table maps Rocket 0.5 concepts to crocket.

| Rocket | crocket |
|---|---|
| `#[get("/hello/<name>/<age>")]` | `[[= http::get("/hello/{name}/{age}")]]` |
| `routes![a, b, c]` | `reflect_routes<^^api>()`: every annotated function in a namespace or class |
| `rocket::build().mount("/", ...)` | `App{}.mount("/", ...)` |
| `#[launch]` | `App::listen({...})` |
| `FromParam` | `crocket::FromParam<T>` |
| Forwarding and `rank = N` | Forwarding and `{.rank = N}` |
| Request guards (`FromRequest`) | Extractors: `crocket::FromRequest<T>` |
| Data guards (`FromData`), `Json<T>` | `Json<T>` (and any extractor with `consumes_body = true`) |
| Query params / `FromForm` for queries | `Query<T>` |
| `Responder` | `crocket::Responder<T>` |
| `Option<T>`, `Result<T, E>`, `status::Created` | `std::optional<T>`, `std::expected<T, E>`, `Created<T>` |
| `.manage(x)` and `&State<T>` | `.manage(x)` and `State<T>` |
| Sentinels (`State<T>` aborts launch if unmanaged) | Ignite checks (same guarantee, plus route conflicts and fairing checks) |
| Fairings (`on_ignite`, `on_request`, `on_response`, `on_shutdown`) | Fairings, same names, as plain structs |
| `async fn` handlers | Handlers returning `Task<T>` |
| `rocket::local::blocking::Client` | `LocalClient` |
| `Shutdown` and grace period | SIGINT/SIGTERM drain with `drain_timeout` |

Two things crocket adds that Rocket has no direct equivalent for:
- **Controllers:** classes whose member functions are routes, run on a managed instance.
- **A production kit in the box:** request ids, structured logs, Prometheus metrics,
  `/healthz` and `/readyz`, deadlines with cancellation, and resource pools.

Things Rocket has that crocket does not (yet) are listed at the end of this section.

### Routing and dynamic parameters

A route is a function with exactly one route annotation. Each `{capture}` in the path
binds to the parameter with the same name. That parameter's type decides whether the
segment is acceptable. If parsing fails, the handler is not called.

```cpp
namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string {
  return std::format("Hello, {} year old named {}!", age, name);
}

}  // namespace api
```

`/hello/Ada/36` calls `hello`. `/hello/Ada/400` does not, because 400 isn't a
`uint8_t`. It returns 404 `path.invalid` instead.

A mismatch between the path and the signature is a **compile error**, not a runtime
surprise:

```
error: static assertion failed: crocket: handler bad::hello [GET /hello/{name}/{age}]:
  path capture '{age}' has no parameter named 'age'; did you mean parameter 'years'?
  (parameters: name, years)
```

Built-in path types are `std::string_view`, `std::string`, `bool`, all integer types
(range-checked) and floating-point types. Add your own by specializing `FromParam`:

```cpp
struct Hex { std::uint32_t value; };

template <>
struct crocket::FromParam<Hex> {
  static std::optional<Hex> parse(std::string_view s) {
    std::uint32_t v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return Hex{v};
  }
};

[[= http::get("/color/{rgb}")]]
auto color(Hex rgb) -> std::string;   // /color/ff8000 -> 200, /color/xyz -> 404
```

Routes for every method: `http::get`, `head`, `post`, `put`, `patch`, `del`, `options`.

- **HEAD** falls back to the GET route and drops the body.
- **Wrong method** on a path that exists gets 405 with an `Allow` header.

### Forwarding and ranks

As in Rocket, a path that fails to parse doesn't stop routing; the request is
*forwarded* to the next candidate route. Candidates are tried in this order:

1. Lower `rank` first.
2. Within a rank, literal segments before captures.
3. Then mount order.

If every candidate forwards, the response is 404 `path.invalid`.

```cpp
[[= http::get("/user/{id}", {.rank = 1})]]
auto user_by_id(std::uint64_t id) -> std::string;            // /user/42

[[= http::get("/user/{name}", {.rank = 2})]]
auto user_by_name(std::string_view name) -> std::string;     // /user/ada
```

Two routes with the same method, path shape and rank are a conflict. They fail at
ignite with both handlers named.

### Request guards: extractors

Every parameter that isn't a path capture is an **extractor**. Extractors are
crocket's request guards. They run before the handler, and if one fails, its
`ApiError` becomes the response and the handler never runs.

| Extractor | Gives you | Fails with |
|---|---|---|
| `Json<T>` | Request body decoded into `T` via reflection | 415 `json.unsupported_media_type`, 422 `json.invalid` / `json.path_mismatch` / `json.read_only` |
| `Query<T>` | Query string mapped onto the members of `T` | 422 `query.invalid` |
| `Auth` | Bearer token verified by the managed `Authenticator` | 401 `auth.missing` / `auth.invalid` / verifier's code |
| `State<T>` | The managed `T` | (checked at ignite, never at request time) |
| `Deadline` | Absolute deadline plus a `std::stop_token` | (none) |
| `RequestId` | The request's id | (none) |
| `const Request&` / `Response&` | Raw access, when you need it | (none) |

A custom guard is a `FromRequest` specialization. This is Rocket's `ApiKey` example in
crocket:

```cpp
struct ApiKey { std::string value; };

template <>
struct crocket::FromRequest<ApiKey> {
  static constexpr std::string_view kind = "api_key";   // label in metrics
  static constexpr bool consumes_body = false;
  static std::expected<ApiKey, ApiError> extract(Request& rq) {
    auto key = rq.header("x-api-key");
    if (!key) return std::unexpected(ApiError::unauthorized("api_key.missing", "x-api-key header required"));
    if (*key != "s3cret") return std::unexpected(ApiError::forbidden("api_key.invalid", "unknown API key"));
    return ApiKey{std::string(*key)};
  }
};

[[= http::get("/sensitive")]]
auto sensitive(ApiKey key) -> std::string;   // 401 without the header, 403 with a wrong one
```

A parameter type that is neither a path type nor an extractor is a compile error that
names the parameter and its type. The same applies to a handler with two body-consuming
extractors.

Failures are counted per route and extractor kind in
`crocket_extractor_failures_total{route,kind}`.

### Body data and queries

```cpp
struct NewUser { std::string email; std::optional<std::string> name; };
struct Page    { int page = 1; std::optional<std::string> q; };

[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth) -> Result<Created<User>, ApiError>;

[[= http::get("/search")]]
auto search(Query<Page> p) -> std::string {           // /search?page=2&q=rust
  return std::format("page {} of results for {}", p->page, p->q.value_or("*"));
}
```

JSON mapping is driven by reflection, with no derive or registration step. It handles:

- aggregates, nested aggregates, `std::vector`, `std::array`, string-keyed maps and
  `std::optional`;
- missing members, which are allowed if they are `optional` or have a default member
  initializer.

Errors say exactly what went wrong: `field 'email': expected string, got number`.

### One struct for create and update: `json::from_path`

A resource's id is usually assigned by the server on `POST /users` and named by the URL
on `PUT /users/{id}`. Instead of writing two structs, mark the id with
`[[= json::from_path]]`:

```cpp
struct User {
  [[= json::from_path]] std::optional<std::uint64_t> id;
  std::string email;
  std::optional<std::string> name;
};

[[= http::post("/users")]]
auto create(Json<User> body, State<Db> db) -> Created<Json<User>>;        // body->id is empty

[[= http::put("/users/{id}")]]
auto replace(std::uint64_t id, Json<User> body, State<Db> db) -> Json<User>;   // body->id == id
```

`Json<T>` applies the rule per route, after decoding:

| Route | Body `id` | Result |
|---|---|---|
| Has an `{id}` capture (PUT, PATCH) | Absent or `null` | Filled from the URL |
| | Same value as the URL | Accepted, so a client can PUT back what it GOT |
| | Different value | 422 `json.path_mismatch` |
| No `{id}` capture (POST) | Absent or `null` | Stays empty for the server to assign |
| | Any value | 422 `json.read_only` |

The field is matched to the capture by name, and responses encode it like any other
member.

Some constraints are checked at compile time. The member must be:
- `std::optional<P>`, because it is absent on routes without the capture;
- of a type `P` that can be parsed from a path segment (any `FromParam` type, e.g. a
  string slug);
- equality-comparable, so it can be checked against the URL.

Only members of the top-level body struct are checked. Nested structs and the elements
of `Json<std::vector<T>>` bodies are decoded as usual.

### Responders

The return type is the response. Built-in responders:

| Return type | Response |
|---|---|
| `std::string`, `std::string_view`, `const char*` | 200 `text/plain` |
| `Json<T>` | 200 `application/json` |
| `std::optional<T>` | `T`, or 404 `not_found` when empty |
| `std::expected<T, ApiError>` (alias `Result<T>`) | `T`, or the error envelope |
| `Result<void>` / `void` | 204 |
| `Created<T>`, `Accepted<T>` | 201 / 202, with an optional `Location` header |
| `Status<T>` | Runtime status plus payload |
| `NoContent` | 204 |
| `Response` | Exactly what you built |
| `Task<T>` | Any of the above, produced asynchronously |

`T` can be any responder or any JSON-encodable type. A return type that is neither is a
compile error.

```cpp
[[= http::post("/items")]]
auto add(Json<Message> msg, State<Store> db) -> Created<Json<Message>> {
  auto id = db->insert(msg->contents);
  return {Json<Message>{*msg}, std::format("/items/{}", id)};   // 201, location: /items/1
}

[[= http::get("/items/{id}")]]
auto item(std::uint64_t id, State<Store> db) -> std::optional<Json<Message>>;   // 404 if absent

[[= http::get("/teapot")]]
auto teapot() -> Status<std::string> { return {418, "short and stout"}; }
```

Custom responders are a `Responder` specialization:

```cpp
struct Csv { std::vector<std::vector<std::string>> rows; };

template <>
struct crocket::Responder<Csv> {
  static void respond(Csv&& csv, const Request&, Response& rs) {
    for (auto& row : csv.rows) {
      for (std::size_t i = 0; i < row.size(); ++i) rs.body += (i ? "," : "") + row[i];
      rs.body += '\n';
    }
    rs.set_content_type("text/csv");
  }
};

[[= http::get("/report.csv")]]
auto report() -> Csv { return {{{"name", "age"}, {"Ada", "36"}}}; }
```

### Errors

Every error, whether from an extractor, a responder, the router or an exception, uses
one JSON envelope and carries the request id:

```json
{"error":{"code":"user.email","message":"email must contain '@'","request_id":"5f0c…"}}
```

`ApiError{status, code, message, detail}` is the type behind it:

- `code` is a stable, dotted identifier that clients can switch on.
- `detail` goes to the logs only, never to the client. The one exception is the
  [dev profile](#dev-profile), which adds it to error bodies.
- An uncaught exception becomes 500 `internal`. Its `what()` is logged, never sent.
- A response header with CR, LF or NUL in its value, or an invalid name, also becomes
  500 `internal`, so a `Location` built from user input cannot inject headers. If an
  `on_response` fairing adds such a header, the engine drops it and reports it on stderr.

### Managed state and sentinels

`manage` a value once and ask for it with `State<T>` anywhere. As with Rocket's
sentinels, a route that needs state nobody manages **fails at ignite**, before the
server accepts a single connection:

```cpp
App{}.mount("/", reflect_routes<^^api>()).ignite();
```
```
ignite failed:
  - GET /items/{id} (api::item) requires State<Store>, but Store is not managed; add .manage(Store{...})
```

Ignite also rejects conflicting routes and anything a fairing's `on_ignite` flags.
`listen` adds one more check: TLS certificate and key files must exist and be readable.
Every problem is reported at once, not just the first.

Managed state is destroyed in reverse `manage` order at shutdown, after requests have
drained. A managed type with a `bool ready() const` member, such as `Pool<T>`, is
automatically part of `/readyz`.

```cpp
Pool<Conn> pool(8, [&] { return Conn{table}; }, {.max_wait = 2s});

[[= http::get("/users/{id}")]]
auto get_user(std::uint64_t id, State<Pool<Conn>> db, Deadline d) -> Result<std::optional<Json<User>>> {
  auto conn = db->checkout(d);             // waits until the deadline; 503 db.busy after that
  if (!conn) return std::unexpected(conn.error());
  /* ... */
}
```

### Controllers

A class can be a route scope. Static members are ordinary handlers. Non-static members
run on the instance you `manage`, and ignite checks that you did.

```cpp
class Info {
 public:
  explicit Info(std::string version) : version_(std::move(version)) {}

  [[= http::get("/version")]]
  auto version() const -> std::string { return version_; }

 private:
  std::string version_;
};

app.manage(Info{"1.4.2"}).mount("/", reflect_routes<^^Info>());
```

### Fairings

Fairings are crocket's structured middleware, with Rocket's hook names. A fairing is
any movable type that has one or more of these members; there is no trait to implement
and no `info()`:

| Hook | When | Can |
|---|---|---|
| `void on_ignite(Ignite&)` | During `ignite()`, in attach order | Inspect state, config and routes; `ig.fail("...")` aborts launch |
| `void on_request(Request&)` | Before routing, in attach order | Rewrite the request |
| `std::optional<Response> on_request(Request&)` | Same | Also answer immediately and stop the chain |
| `void on_response(const Request&, Response&)` | After the response exists, in **reverse** attach order | Rewrite the response |
| `void on_shutdown()` | After the drain, in reverse attach order, before state is destroyed | Flush, close, say goodbye |

Differences from Rocket:

- **`on_request` can answer the request.** Returning a `Response` stops the chain, which
  is useful for maintenance mode and CORS preflights. Rocket's request fairings can only
  modify the request.
- **`on_response` runs in reverse attach order.** Fairings nest like layers: the first
  attached sees the request first and the response last.
- **`on_response` runs on every response, early rejections included.** That covers the
  engine's 413, 503 and 504, so logs and metrics never miss a request.

A response timer (compare Rocket's `RequestTimer`):

```cpp
struct Timer {
  void on_response(const Request& rq, Response& rs) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - rq.received).count();
    rs.headers.set("x-response-time", std::format("{} us", us));
  }
};
```

Maintenance mode, answering early:

```cpp
struct Maintenance {
  std::shared_ptr<std::atomic<bool>> on = std::make_shared<std::atomic<bool>>(false);

  std::optional<Response> on_request(Request& rq) {
    if (!*on || rq.path == "/healthz") return std::nullopt;   // carry on
    Response rs;
    rs.status = 503;
    rs.headers.set("retry-after", "120");
    rs.body = "down for maintenance\n";
    return rs;                                               // handler never runs
  }
};
```

Launch checks and cleanup:

```cpp
struct RequireKeyInProd {
  bool prod;
  void on_ignite(Ignite& ig) {
    if (prod && !ig.state<ApiKey>()) ig.fail("RequireKeyInProd: production needs a managed ApiKey");
  }
  void on_shutdown() { std::puts("bye"); }
};
```

A custom 404 page, standing in for a Rocket `#[catch(404)]`:

```cpp
struct NotFoundPage {
  void on_response(const Request& rq, Response& rs) {
    if (rs.status == 404 && !rq.header("accept").value_or("").contains("json")) {
      rs.body = "<h1>404</h1><p>Nothing here.</p>";
      rs.set_content_type("text/html; charset=utf-8");
    }
  }
};

App{}.attach(Timer{}).attach(Maintenance{}).attach(RequireKeyInProd{false}).attach(NotFoundPage{});
```

Fairings run concurrently on worker threads, so keep any shared state thread-safe.
`Maintenance` above uses an atomic behind a `shared_ptr` so you can keep a handle to
flip it.

Built-in fairings:

- **`Logger`:** one JSON line per request with id, method, route template (never the
  raw path), handler, status, error code, duration, bytes, protocol and log-only detail.
  Also lines at ignite and shutdown. Pass a sink to send lines elsewhere.
- **`Cors`:** deny-by-default CORS.
  - `Cors::allow_origins({...})` with `allow_methods`, `allow_headers`,
    `expose_headers`, `allow_credentials` and `max_age`.
  - Answers preflights itself, or 403 `cors.denied` for origins not on the list.
  - Refuses at ignite to combine credentials with `*`.
- **`Metrics`:** Prometheus text at `GET /metrics`, labelled by route template.
  - `crocket_http_requests_total` and a duration histogram.
  - `crocket_http_requests_in_flight`.
  - `crocket_extractor_failures_total`.

### Async handlers

Return `Task<T>` from the same annotations. Inside, `co_await` other tasks or anything
awaitable.

```cpp
auto compute_fib(std::uint32_t n) -> Task<std::uint64_t>;   // any awaitable works

[[= http::get("/fib/{n}")]]
auto fib(std::uint32_t n) -> Task<Json<std::uint64_t>> {
  co_return Json<std::uint64_t>{co_await compute_fib(n)};
}
```

### Deadlines and cancellation

Every request has a deadline (`Config::request_timeout`, default 30 s) and a stop
token. The token fires when the deadline passes or the client disconnects.

- **Extractors and pools** such as `Pool::checkout` honour the token automatically.
- **Handlers doing long work** can take a `Deadline` parameter and poll `expired()`.
- **At the deadline** the client gets 504 `deadline.exceeded` regardless of what the
  handler is doing.

### Testing

`LocalClient` dispatches requests through the full pipeline without sockets: fairings,
routing, extractors, responders and error mapping. It plays the role of Rocket's
`local::blocking::Client`.

```cpp
App app = make_app();
LocalClient client(app);

auto res = client.post("/users").bearer("alice").json(R"({"email":"a@example.com"})").dispatch();
assert(res.status == 201);
assert(*res.headers.get("location") == "/users/1");

auto bad = client.get("/hello/Ada/400").dispatch();
assert(bad.status == 404 && bad.error_code == "path.invalid");
```

`tests/acceptance.cpp` is a larger example. Compile-time guarantees are tested too:
`tests/compile_fail/` contains programs that must not compile, each checked for the
expected diagnostic.

### Launching

```cpp
Config cfg;
cfg.request_timeout = 10s;        // 504 deadline.exceeded
cfg.max_body_bytes  = 1 << 20;    // 413 body.too_large
cfg.max_in_flight   = 1024;       // 503 server.busy
cfg.drain_timeout   = 5s;         // graceful shutdown budget
cfg.debug_routes    = true;       // GET /__routes lists every route

return App{cfg}
    .manage(std::move(pool))
    .attach(Logger{})
    .attach(Metrics{})
    .mount("/api", reflect_routes<^^api>())
    .listen({.port = 8443, .tls_cert = "cert.pem", .tls_key = "key.pem"});   // h2 via ALPN
```

What you get:

- **Protocols:** HTTP/1.1, HTTPS, and HTTP/2 when the client negotiates it over TLS.
- **Built-in routes:** `GET /healthz` and `GET /readyz`.
- **Request ids:** an `x-request-id` on every response, honouring the client's value
  when valid.
- **Graceful shutdown on SIGINT/SIGTERM:** stop accepting, drain in-flight requests,
  run `on_shutdown`, then destroy managed state.

The threading model and limits are in [docs/ENGINE.md](docs/ENGINE.md).

### Dev profile

`Config::dev()` is for running on your own machine. Choose it in code, or read
`CROCKET_PROFILE` with `Config::from_env()`, which gives `dev()` for `dev`, the release
defaults when unset or `release`, and throws `std::invalid_argument` for anything else:

```cpp
return App{Config::from_env()}   // CROCKET_PROFILE=dev ./my_app
    .attach(Logger{})
    .mount("/", reflect_routes<^^api>())
    .listen({.port = 8000});
```

Compared with the release defaults, the dev profile:

- **Error bodies carry `detail`**, including an uncaught exception's `what()`, so you
  see the cause in the client without reading logs. Release never sends it.
- **Logs are readable lines** from `Logger`, coloured on a terminal unless `NO_COLOR`
  is set: `14:02:11.504 GET /hello/Ada/400 404 0.21ms api::hello path.invalid: …`.
- **`listen()` prints every route** before it starts.
- **`GET /__routes` is on**, the request deadline is an hour (room for a debugger
  breakpoint), and the drain on shutdown is one second.
- **An empty `ListenOptions::host` binds `127.0.0.1`** instead of `0.0.0.0`.

### Not (yet) in crocket

Rocket features without an equivalent today:

| Rocket feature | crocket today |
|---|---|
| `Form<T>`, `TempFile` | Use `Json<T>` |
| Cookies and private cookies | Read headers via `Request`; set via `Response` |
| Templates | Return `std::string` or a custom responder |
| `FileServer` | Not implemented |
| Typed URIs (`uri!`) | Not implemented |
| WebSockets, SSE and streaming responses | Not implemented; responses are fully buffered |
| `Shield` security headers | Write a small `on_response` fairing |
| Error catchers (`#[catch]`) | `ApiError` plus an `on_response` fairing, as in `NotFoundPage` above |
| `on_liftoff` | Not implemented |
| Config profiles (`Rocket.toml` / Figment) | A release and a [dev profile](#dev-profile) chosen with `CROCKET_PROFILE`; other settings are plain structs, read from env or files yourself (see `examples/serve.cpp`) |
| Mutual TLS | Not implemented |

## Building

The toolchain used for development is GCC 16.2 (`-std=c++26 -freflection`, added
automatically as a `PUBLIC` compile option of the `crocket` target), CMake 4.2 and
Ninja on Ubuntu 26.04. You also need OpenSSL development headers, because lws is built
with TLS.

GCC 16.2 or later is required. Ubuntu 26.04's `g++-16` package is a pre-release
snapshot (`16-20260322`) whose reflection bugs break any translation unit that mounts
routes. Its errors start with `accessing uninitialized member 'crocket::detail::Binding::kind'`.

CMake links with lld when it can, then mold, then the default linker. Each candidate
is tried with a real test link, because mold 2.40 cannot link with GCC 16.

libwebsockets 4.3.5 declares a `cmake_minimum_required` that CMake 4 rejects. The
top-level `CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` before fetching it.

Options:

- `CROCKET_BUILD_EXAMPLES` builds `crocket_hello` and `crocket_serve`.
- `CROCKET_BUILD_TESTS` builds the tests.

Both are ON when crocket is the top-level project and OFF when it is a dependency.

## Working on crocket

`./dev` is the entry point for developing crocket itself. Applications using crocket
don't need it.

```bash
./dev setup        # apt packages (asks for sudo)
./dev gcc          # build GCC 16.2 into ~/.local/gcc-16.2 (about 10 minutes on 32 cores)
./dev watch hello  # run an example; rebuild and restart it on every save
./dev check        # release build plus the full ctest, before calling a change done
```

`./dev help` lists every command. CI (`.github/workflows/ci.yml`) runs `./dev setup`,
`./dev gcc` and `./dev check` in an `ubuntu:26.04` container on every push to `main`
and every pull request, and caches the GCC build. Builds go to `build-dev/` (Debug) and `build/`
(release), configured from `CMakePresets.json`. `./dev` picks the compiler in this
order: `$CROCKET_CXX`, GCC 16.2 in `~/.local/gcc-16.2`, then `g++-16`.

## Tests

| Test | What it checks |
|---|---|
| `acceptance` | The request pipeline in-process via `LocalClient`, with no sockets. Covers acceptance items 1–4 and 6, plus routing, responders, request ids, log lines, metrics, health checks, CORS, pools, the dev profile and header validation. |
| `compile_fail.*` | Programs that must not compile. Covers acceptance item 5, where the `{age}` vs `years` diagnostic must name both identifiers, plus other misuses and a control file that must compile. |
| `consumer` | `tests/consumer`, a small application that adds crocket with FetchContent, builds with only `crocket::crocket` linked (no C++ standard of its own), and serves one request. It also fails if crocket's targets or lws cache entries leak into the application. |
| `tls_h2` | Real sockets, self-signed TLS, ALPN h2 and http/1.1, and graceful drain (acceptance item 7). Needs `openssl` and a curl built with HTTP/2. Uses ports 18443 and 8000. |

## `crocket_serve` configuration

`crocket_serve` reads its settings from environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `CROCKET_PROFILE` | `release` | `dev` for the [dev profile](#dev-profile) |
| `CROCKET_HOST` | `0.0.0.0` (`127.0.0.1` in dev) | Address to listen on |
| `CROCKET_PORT` | `8000` | Port to listen on |
| `CROCKET_TLS_CERT`, `CROCKET_TLS_KEY` | unset | Enable TLS; both must be set |
| `CROCKET_DEBUG_ROUTES` | unset | Set to any value to expose `GET /__routes` |

## Differences from REQUIREMENTS.md

- A missing bearer token is 401 `auth.missing`, and a non-Bearer scheme is 401
  `auth.invalid`. The requirements list `auth.expired`; the `Authenticator` still
  returns `auth.expired` for expired tokens.
- `Json<T>` with a non-JSON `content-type` is 415 `json.unsupported_media_type`.
  Malformed JSON is 422 `json.invalid` as specified.
- `Task<T>` handlers are awaited synchronously on a worker thread (see
  [docs/ENGINE.md](docs/ENGINE.md)).
- Over HTTP/2, a client-supplied `X-Request-Id` is not visible, because of a
  libwebsockets 4.3.5 limitation, so a new id is generated. See
  [docs/ENGINE.md](docs/ENGINE.md).

## License

crocket is released under the [MIT license](LICENSE).

It links [libwebsockets](https://libwebsockets.org) statically into your program.
libwebsockets is MIT-licensed, with a few files under BSD licenses, so a binary you
distribute must also include its license notice (`LICENSE` in the libwebsockets
source). OpenSSL is linked dynamically under the Apache License 2.0.
