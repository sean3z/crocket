# crocket

A C++ web framework in the same spirit as [Rocket](https://rocket.rs) for Rust. Not a port.

Handlers are functions. Path captures and extractors are arguments. The return value is the response. Routes are declared with C++26 annotations and discovered with reflection. The wire layer is [h2o](https://github.com/h2o/h2o) (HTTP/1.1, HTTP/2, TLS).

```cpp
namespace api {

[[= http::get("/hello/{name}/{age}")]]
auto hello(std::string_view name, std::uint8_t age) -> std::string;

}  // namespace api

crocket::build()
  .attach(Logger{})
  .mount("/", reflect_routes<^^api>())
  .launch({.host = "0.0.0.0", .port = 8000});
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
own examples and tests are not built, and h2o stays private: it is not in your
headers or your CMake cache.

You need:

- **GCC 16.2 or later.** Ubuntu 26.04's `g++-16` package is too old (see
  [Building](#building)). Clang is not tested yet.
- **CMake 3.28 or later**, and Ninja or Make.
- **OpenSSL and zlib development headers** (`libssl-dev`, `zlib1g-dev`), and `perl`,
  which h2o's build runs to generate a header.
- **Network access on the first configure**, to fetch h2o (a 7 MB tarball) from GitHub.
  Set `FETCHCONTENT_SOURCE_DIR_H2O` to a local checkout to build offline.

There are no `install()` rules yet, so `find_package(crocket)` does not work.

## Rocket, in C++

If you know [Rocket](https://rocket.rs), almost every concept carries over. The table maps Rocket 0.5 concepts to crocket.

| Rocket | crocket |
|---|---|
| `#[get("/hello/<name>/<age>")]` | `[[= http::get("/hello/{name}/{age}")]]` |
| `routes![a, b, c]` | `reflect_routes<^^api>()`: every annotated function in a namespace or class |
| `rocket::build().mount("/", ...)` | `crocket::build().mount("/", ...)` |
| `#[launch]` | `.launch({...})` |
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

Three things crocket adds that Rocket has no direct equivalent for:
- **Controllers:** classes whose member functions are routes, run on a managed instance.
- **gRPC:** unary methods next to your HTTP routes, with protobuf messages that are
  plain structs and a `.proto` generated from them (see [gRPC and protobuf](#grpc-and-protobuf)).
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

Routes are compiled at ignite into a tree keyed by path segment, so finding the
candidates takes the same time with 10 routes or 1,000 and allocates nothing. A route
may have up to 64 segments and 16 captures; longer paths are 404.

### Request guards: extractors

Every parameter that isn't a path capture is an **extractor**. Extractors are
crocket's request guards. They run before the handler, and if one fails, its
`ApiError` becomes the response and the handler never runs.

| Extractor | Gives you | Fails with |
|---|---|---|
| `Json<T>` | Request body decoded into `T` via reflection (see [JSON](#json)) | 415 `json.unsupported_media_type`, 422 `json.invalid` / `json.validation` / `json.unknown_field` / `json.duplicate_key` / `json.limit_exceeded` / `json.path_mismatch` / `json.read_only` |
| `Query<T>` | Query string mapped onto the members of `T` | 422 `query.invalid` |
| `Header<"name", T>` | Header `name` parsed as `T` (default `std::string`; `std::optional<U>` if it may be absent) | 400 `header.missing` / `header.invalid` |
| `Auth` | Bearer token verified by the managed `Authenticator` | 401 `auth.missing` / `auth.invalid` / verifier's code |
| `State<T>` | The managed `T` | (checked at ignite, never at request time) |
| `Deadline` | Absolute deadline plus a `std::stop_token` | (none) |
| `RequestId` | The request's id | (none) |
| `Client` | The client's address and scheme (see [Behind a proxy](#behind-a-proxy)) | (none) |
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

Single headers are read the same way, by name in the type:

```cpp
[[= http::get("/reports")]]
auto reports(Header<"x-tenant"> tenant,                       // required: 400 header.missing
             Header<"x-page-size", std::optional<int>> size)  // optional, parsed like a path value
    -> std::string {
  return std::format("{} {}", *tenant, size->value_or(50));
}
```

Every request header reaches the handler, over HTTP/1.1 and HTTP/2 alike.

### JSON

Any plain struct works as JSON as it is, with no annotations, derive macros or
registration:

```cpp
struct NewUser {
  std::string email;                // required
  std::optional<std::string> name;  // may be absent or null
  int age = 0;                      // may be absent: keeps its default
};

[[= http::post("/users")]]
auto create(Json<NewUser> body) -> Json<NewUser> { return body; }
```

By default, JSON names are the member names, a member is required unless it is
`optional` or has a default initializer, unknown fields are ignored, and empty optionals
are written as `null`. [Annotations](#annotations) change those defaults where you need
to, and [validation](#validation) adds checks, but neither is required.

`Json<T>` reads the body straight into `T`, with no document tree in between, and a
responder writes `T` back out. The strict reading rules below apply to every struct,
annotated or not.

| C++ | JSON |
|---|---|
| `bool`, integers, floating point | `true`/`false`, number (NaN and infinity are written as `null`) |
| `std::string` | string |
| an enum | string: the enumerator's name |
| an aggregate struct | object, one member per data member |
| `std::optional<T>` | `T` or `null` |
| `std::vector<T>`, `std::array<T, N>` | array (`std::array`: exactly `N` elements) |
| `std::map<std::string, T>` | object |
| `std::variant<T...>` | untagged by default; see `json::tag` |
| `std::chrono::sys_time<D>` | RFC 3339 `"2026-10-05T14:03:07.250Z"`, with digits for `D`'s precision; `"2026-10-05"` when `D` is days |
| `std::chrono::year_month_day` | `"2026-10-05"` |
| `std::chrono::duration<R, P>` | number of ticks |
| `json::Value` | any JSON |

Errors name the field with a JSON Pointer:
`field '/items/2/qty': expected integer, got string`.

Reading is strict, because a lenient parser is one that disagrees with the next one in
line:

- **UTF-8 is checked.** Overlong forms, surrogates and stray bytes are 422 `json.invalid`.
  Output is always valid UTF-8: a `std::string` holding invalid bytes is written with
  U+FFFD in their place.
- **Duplicate keys are rejected** (422 `json.duplicate_key`) in every object, including
  ones the struct ignores. `{"a":1,"\u0061":2}` counts as a duplicate.
- **Every dimension is limited** by `Config::json` (`json::ReadOptions`): nesting depth
  (64), string length (1 MiB), members per object (1024) and elements per array
  (100,000). Exceeding one is 422 `json.limit_exceeded`.
- **Integers keep every bit.** `std::uint64_t` reads all 64 bits, and a value that does
  not fit the member's type is an error, never a rounded number.

#### Annotations

```cpp
struct [[= json::rename_all(json::camel_case), = json::deny_unknown_fields]] Signup {
  std::string display_name;                                      // "displayName"
  [[= json::rename("e-mail")]] std::string email;
  [[= json::as_string]] std::uint64_t referrer_id = 0;           // "9007199254740993"
  [[= json::omit_null]] std::optional<std::string> note;         // left out when empty
  [[= json::unix_seconds]] std::chrono::sys_seconds joined{};    // 1759672987
  [[= json::tag("type")]] std::variant<Card, BankTransfer> payment;
};
```

| Annotation | On | Effect |
|---|---|---|
| `json::rename("name")` | member, enumerator, struct | The JSON name. On a struct, it is the struct's `json::tag` value. |
| `json::rename_all(json::camel_case)` | struct, enum | Names for every member or enumerator without a `rename`: `snake_case`, `camel_case`, `pascal_case`, `kebab_case` or `screaming_snake_case`. |
| `json::deny_unknown_fields` | struct | A member `T` does not have is 422 `json.unknown_field` instead of being ignored. `Config::json.deny_unknown_fields` turns this on everywhere. |
| `json::omit_null` | optional member, struct | An empty optional is left out instead of written as `null`. |
| `json::as_string` | integer member, struct | Written as a string, so JavaScript clients (exact only up to 2^53) see every digit. Reading accepts a string or a number. |
| `json::unix_seconds`, `json::unix_millis` | `sys_time` member | An integer timestamp instead of RFC 3339. |
| `json::tag("type")` | `std::variant` of structs | An internally tagged union: `{"type":"Card","number":"…"}`. The tag can be anywhere in the object. |

An untagged `std::variant` reads the first alternative whose JSON type fits and that
reads without error, so `std::variant<std::int64_t, std::string>` takes `5` or `"5"`.
`std::monostate` is `null`. An alternative that fails is re-read as the next one, and
when struct alternatives nest inside each other, the re-reading multiplies with depth.
Re-reading is capped at 8 times the body's size, and beyond that the request is 422
`json.limit_exceeded`. Use `json::tag` for unions of structs, especially recursive ones.

Two members with the same JSON name, or an annotation on a member it cannot apply to,
are compile errors.

#### Validation

```cpp
struct NewUser {
  [[= json::email]] std::string email;
  [[= json::min_len(1), = json::max_len(64)]] std::string name;
  [[= json::pattern("^[a-z0-9_]{3,20}$")]] std::string handle;
  [[= json::min(13), = json::max(130)]] std::optional<int> age;
  [[= json::max_len(10)]] std::vector<std::string> tags = {};
};
```

| Annotation | On | Checks |
|---|---|---|
| `json::min(n)`, `json::max(n)` | numbers | Inclusive bounds. |
| `json::min_len(n)`, `json::max_len(n)` | strings, arrays, maps | Length in code points (strings) or elements. |
| `json::pattern("re")` | strings | A regular expression that must match somewhere; anchor it with `^…$`. |
| `json::email` | strings | An address of the form `local@domain.tld`, within RFC 5321's lengths. |

Checks on an `optional` apply when it holds a value. A failing check does not stop
reading. Every failure is collected and returned as one 422 `json.validation`, with an
`errors` entry per field:

```json
{"title":"Unprocessable Content","status":422,
 "detail":"field '/email': must be an email address; field '/age': must be at least 13",
 "code":"json.validation","request_id":"5f0c…",
 "errors":[{"pointer":"/email","detail":"must be an email address"},
           {"pointer":"/age","detail":"must be at least 13"}]}
```

`json::pattern` compiles the expression when the program compiles, so a bad pattern is a
compile error. It matches in time linear in the input, without backtracking, so no
request can make it slow. The syntax is a subset of ECMAScript's: classes, `\d \w \s`,
anchors, groups, `|` and the usual quantifiers, but no backreferences or lookaround (see
[`detail/regex.hpp`](include/crocket/detail/regex.hpp)). `json::validate(value)` runs the
same checks on a value built in code.

Outside a handler, `json::read(text, value)`, `json::from_string<T>(text)` and
`json::to_string(value)` do the same mapping, and `json::parse(text)` returns a
`json::Value` when the shape is not known.

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
| `std::expected<T, ApiError>` (alias `Result<T>`) | `T`, or the error as a problem+json body |
| `Result<void>` / `void` | 204 |
| `Created<T>`, `Accepted<T>` | 201 / 202, with an optional `Location` header |
| `Status<T>` | Runtime status plus payload |
| `NoContent` | 204 |
| `Cacheable<T>` | `T` with `Last-Modified` and byte ranges (see [Caching and ranges](#caching-and-ranges)) |
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

### Caching and ranges

Every `200` answer to `GET` or `HEAD` carries an `ETag`, a hash of the body, unless the
handler set its own. A client that sends it back in `If-None-Match` gets `304 Not
Modified` with no body, so unchanged responses cost no bandwidth. Nothing needs
configuring.

Wrap the payload in `Cacheable` for the rest:

```cpp
[[= http::get("/reports/{id}")]]
auto report(std::uint64_t id, State<Store> store) -> Cacheable<std::string> {
  auto r = store->find(id);
  return {r.contents, {.last_modified = r.updated_at, .ranges = true}};
}
```

- **`last_modified`** is sent as `Last-Modified`, and answers `If-Modified-Since` with
  304 when the client's copy is as new. `If-None-Match`, when sent, decides instead.
- **`ranges`** serves one byte range (`Range: bytes=0-499`, `500-` or
  `-500`) as `206 Partial Content`. A range past the end is 416 `range.unsatisfiable`,
  and a request for several ranges gets the whole body. `If-Range` makes the range
  conditional on the client still having the current version.

### Errors

Every error, whether from an extractor, a responder, the router or an exception, is an
[RFC 9457](https://www.rfc-editor.org/rfc/rfc9457) problem (`application/problem+json`)
and carries the request id:

```json
{"title":"Unprocessable Content","status":422,"detail":"email must contain '@'",
 "code":"user.email","request_id":"5f0c…"}
```

`ApiError{status, code, message, detail}` is the type behind it:

- `message` is the problem's `detail`, written for the client.
- `code` is a stable, dotted identifier that clients can switch on.
- `errors` lists failing fields as `{"pointer", "detail"}` pairs. `Json<T>` fills it
  from validation and decoding errors.
- `type` is an optional problem-type URI. It is left out when empty, which RFC 9457
  reads as `about:blank`, and `title` is the status's reason phrase.
- `detail` goes to the logs only, never to the client. The one exception is the
  [dev profile](#dev-profile), which adds it to error bodies as `debug`.
- An uncaught exception becomes 500 `internal`. Its `what()` is logged, never sent.
- A response header with CR, LF or NUL in its value, or an invalid name, also becomes
  500 `internal`, so a `Location` built from user input cannot inject headers. If an
  `on_response` fairing adds such a header, the engine drops it and reports it on stderr.

### Managed state and sentinels

`manage` a value once and ask for it with `State<T>` anywhere. As with Rocket's
sentinels, a route that needs state nobody manages **fails at ignite**, before the
server accepts a single connection:

```cpp
crocket::build().mount("/", reflect_routes<^^api>()).ignite();
```
```
ignite failed:
  - GET /items/{id} (api::item) requires State<Store>, but Store is not managed; add .manage(Store{...})
```

Ignite also rejects conflicting routes and anything a fairing's `on_ignite` flags.
`launch` adds one more check: TLS certificate and key files must exist and be readable.
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

crocket::build().attach(Timer{}).attach(Maintenance{}).attach(RequireKeyInProd{false}).attach(NotFoundPage{});
```

Fairings run concurrently on worker threads, so keep any shared state thread-safe.
`Maintenance` above uses an atomic behind a `shared_ptr` so you can keep a handle to
flip it.

Built-in fairings:

- **`Logger`:** one line per request with the request and trace ids, method, route
  template (never the raw path), handler, status, error code, duration, bytes in and
  out, protocol, client address, user agent, authenticated subject and log-only detail.
  Also lines at ignite and shutdown. See [Logging](#logging) for where lines go.
- **`Cors`:** deny-by-default CORS.
  - `Cors::allow_origins({...})` with `allow_methods`, `allow_headers`,
    `expose_headers`, `allow_credentials` and `max_age`.
  - Answers preflights itself, or 403 `cors.denied` for origins not on the list.
  - Refuses at ignite to combine credentials with `*`.
- **`Shield`:** security headers on every response, attached by default:
  `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
  `Content-Security-Policy: default-src 'none'; frame-ancestors 'none'` and
  `Referrer-Policy: no-referrer`, plus `Strict-Transport-Security: max-age=31536000` on
  https (never for localhost). A header the handler set itself is left alone. Attach
  your own to change it, or `Shield::none()` to send nothing:
  ```cpp
  app.attach(Shield{}.set("content-security-policy", "default-src 'self'").remove("x-frame-options"));
  ```
- **`Metrics`:** Prometheus text at `GET /metrics`, labelled by route template.
  - `crocket_http_requests_total` and a duration histogram.
  - `crocket_http_requests_in_flight`.
  - `crocket_extractor_failures_total`.

### Logging

`log::debug`, `log::info`, `log::warn` and `log::error` work anywhere: in a handler, in
code it calls, in a fairing, or outside any request.

```cpp
[[= http::post("/users")]]
auto create(Json<NewUser> body, Auth auth, State<Db> db) -> Result<Created<User>> {
  auto user = db->insert(*body);
  log::info("created user {user_id} on plan {plan}", user.id, body->plan);
  return Created<User>{user, std::format("/users/{}", user.id)};
}
```

```json
{"ts":"2026-10-05T14:03:07.250Z","level":"info","msg":"created user 42 on plan pro",
 "request_id":"5f0c…","trace_id":"4bf92f35…","route":"/users","handler":"api::create",
 "subject":"alice","user_id":42,"plan":"pro"}
```

- **Each `{name}` is a field as well as part of the text.** Arguments may be anything
  JSON can encode, structs included. A count that does not match the placeholders, a
  repeated name, or a name crocket writes itself (`request_id`, `msg`, ...) is a compile
  error. Write `{{` and `}}` for literal braces.
- **The request comes along by itself.** While crocket runs a handler and its fairings,
  every line carries that request's id, trace id (from a W3C `traceparent` header),
  route, handler and authenticated subject, at any depth of the call stack. Outside a
  request, lines carry none of these. A thread the handler starts takes the context
  with it explicitly:
  ```cpp
  auto ctx = log::context();
  std::thread([ctx] { log::Scope scope{ctx}; log::info("indexing {count} rows", n); }).detach();
  ```
- **Lines are written by a background thread**, so a slow terminal or log shipper never
  holds up a request. `LocalClient` waits for a request's lines before `dispatch()`
  returns, and `app.flush_logs()` waits for everything so far.

`Config::log` controls everything:

```cpp
cfg.log.level = log::Level::debug;           // default info; Config::dev() uses debug
cfg.log.sample = 0.1;                        // keep 10% of successful request lines
cfg.log.redact = {"password", "card"};       // fields shown as "[redacted]"
cfg.log.buffer = 10'000;                     // lines waiting for the writer
cfg.log.when_full = log::WhenFull::drop;     // or block
cfg.log.sink = [](std::string_view line) { ship(line); };   // default: stderr
```

| Option | Default | Meaning |
|---|---|---|
| `level` | `info` | Lines below it are skipped, request lines included (successes are `info`, 4xx `warn`, 5xx `error`). |
| `sample` | `1.0` | Share of request lines kept for responses below 400, chosen by request id, so the same request is kept on every instance. Errors are always kept. |
| `redact` | `password`, `secret`, `token`, `authorization`, `cookie`, `api_key` | A field whose name contains one of these, ignoring case, is written as `"[redacted]"`, in the message too and inside structs. |
| `buffer` | `10'000` | Lines waiting for the writer. |
| `when_full` | `drop` | `drop` discards new lines while the buffer is full, then writes a warning with the count and adds it to `crocket_log_lines_dropped_total`. `block` makes the logging thread wait instead. |
| `sink` | stderr | Called on the writer thread with each line. |

In the dev profile, lines are readable text instead of JSON:
`14:03:07.250 INFO  created user 42 on plan pro [5f0c…]`.

### Async handlers

Return `Task<T>` from the same annotations. A plain function and a `Task` differ in where
they run:

- **A plain function may block**: a synchronous database call, a file read, a lock. It
  starts on a worker, so the network threads carry on meanwhile. See
  [adaptive placement](#adaptive-placement) for the fast ones.
- **A `Task` runs on the event loop of its connection**, with no handoff to a worker, so
  it is the fastest kind of handler. While it waits on a `co_await`, the loop serves
  other connections. **It must not block.** It awaits instead, because blocking a loop
  stalls every connection on it. This is the same rule as async code in Rocket, Node
  and FastAPI.

```cpp
[[= http::get("/quote/{sym}")]]
auto quote(std::string sym, State<PriceClient> prices) -> Task<Json<Quote>> {
  auto [price, resolve] = callback<double>();
  prices->fetch(sym, [resolve](double p) { resolve(p); });   // any callback-based client
  co_return Json{Quote{sym, co_await std::move(price)}};
}
```

- **`co_await sleep_for(d)`** waits without holding a thread. It ends early when the
  request is cancelled (deadline, client gone, shutdown), so check `Deadline::expired()`
  afterwards when that matters.
- **`callback<T>()`** adapts a library that reports results through a callback: call
  `resolve(value)` once, from any thread. If every `resolve` is destroyed unused, the
  `co_await` throws and the request is a 500.
- **Other awaitables** work too: another `Task`, or an async client's own awaitable.
- **Code in a task always runs on its connection's event loop.** If a library resumes
  the task on its own thread, it moves back to the loop before continuing. So log lines
  keep their request context, and `Deadline` keeps working.
- **Extractors and `on_request` fairings run on the loop too** for a `Task` route, so an
  `Authenticator` that verifies a token must not block either.
- **Blocking inside a `Task` is reported.** A task that holds its loop for more than
  10 ms is named in a `warn` log line, once, so a synchronous call that slipped in is
  found: `api::quote held its event loop for 48 ms, stalling the other connections on
  it`. Move blocking work into a plain function, or await it.
- **A mutex must not be held across a `co_await`.**

If an `on_request` fairing rewrites a request bound for a `Task` route so that it reaches
a plain function, that request moves to a worker before the function runs. `LocalClient`
waits for an async handler like any other.

### Adaptive placement

A plain function that turns out to be fast doesn't need a worker. Crocket times every
run, and moves a route to the event loops once 1,000 runs in a row have each taken under
100 µs. There it skips the handoff to a worker and back, as a `Task` does. When it turns
out to block there, it moves back to workers: one run over 50 ms does it, and so do 8 runs
over 1 ms within a second. A single run over 1 ms is not enough, because a run's time also
counts moments when the operating system gave the thread's core to something else. It can
try the loops again after 10 s, then 20 s, and so on, so a route that is only sometimes
slow settles on workers. Both moves are logged:

```
INFO  api::get_user runs on the event loops from now on: 1000 runs in a row each took under 100 us
WARN  api::get_user blocked its event loop (48 ms, after 8 slow runs within 1 s), stalling the other connections on it, so it runs on workers again; it may move back after 10 s of fast runs
```

- **Routes that may wait never move.** A route taking a `State<Pool<T>>` stays on workers
  however fast it is, because a checkout is quick until the pool runs dry. A managed type
  of your own can say the same with `static constexpr bool crocket_may_block = true;`.
- **The stalls it allows** are the runs that get a route moved back: a route that has
  been fast 1,000 times and then blocks holds its loop once for a long call, or up to
  8 times for short ones.
- **`GET /__routes`** shows where each route runs now (`"runs_on"`).
- **`Config::adaptive_placement = false`** keeps every plain function on workers.

Timing a run costs two clock reads, about 40 ns.

### Deadlines and cancellation

Every request has a deadline (`Config::request_timeout`, default 30 s) and a stop
token. The token fires when the deadline passes or the client disconnects.

- **Extractors and pools** such as `Pool::checkout` honour the token automatically.
- **Handlers doing long work** can take a `Deadline` parameter and poll `expired()`.
- **At the deadline** the client gets 504 `deadline.exceeded` regardless of what the
  handler is doing.

### gRPC and protobuf

Unary gRPC methods are functions too. Messages are plain structs, encoded as proto3 by
reflection, so the C++ declarations are the schema: there is no `.proto` to keep in sync
and no generated code.

```cpp
struct HelloRequest {
  std::string name;                                     // field 1
  std::optional<std::string> title;                     // field 2
  [[= proto::field(4)]] std::vector<std::string> tags;  // field 4 (3 was retired)
};
struct HelloReply { std::string message; };

namespace greeter {

[[= grpc::rpc]]
auto say_hello(HelloRequest req, Auth auth) -> Result<HelloReply> {
  if (req.name.empty()) return std::unexpected(ApiError::bad_request("hello.name", "name is required"));
  return HelloReply{"Hello, " + req.name};
}

}  // namespace greeter

crocket::build()
    .mount("helloworld", reflect_routes<^^greeter>(), Mode::Grpc)  // POST /helloworld.Greeter/SayHello
    .mount("/api", reflect_routes<^^api>())                         // HTTP routes on the same port
    .launch({.port = 50051, .h2_prior_knowledge = true});
```

- **Mounting:** `Mode::Grpc` makes the first argument of `mount` a protobuf package
  (`"helloworld"`, `"acme.v1"`, or `""` for none) instead of a path. The service is the
  namespace or class and the method the function, both in PascalCase
  (`greeter::say_hello` is `helloworld.Greeter/SayHello`). Mounting gRPC methods without
  `Mode::Grpc`, or HTTP routes with it, fails ignite.
- **Field numbers** follow declaration order from 1. `[[= proto::field(n)]]` pins a member
  to `n`, and the members after it continue from `n + 1`. Duplicate or reserved numbers
  are compile errors. Renumbering breaks existing clients, so pin numbers before
  reordering or removing members.
- **Types:** `bool`, `intN_t`, `uintN_t`, `float`, `double`, `std::string`, `proto::Bytes`,
  enums, nested structs, and `std::optional`, `std::vector` and `std::map` of those.
  Unknown fields are skipped when decoding.
- **Parameters:** at most one request message (none means `google.protobuf.Empty`), plus
  any extractor that does not read the body: `Auth`, `State<T>`, `Deadline`, `Header<...>`
  and so on.
- **Returns:** a message, `Result<T>`, `std::optional<T>` (empty is `NOT_FOUND`), `void`
  (`Empty`), or a `Task` of one of those. Only unary calls are supported: no streaming.
- **Errors:** an `ApiError` becomes a gRPC status derived from its HTTP status (401
  `UNAUTHENTICATED`, 404 `NOT_FOUND`, 422 `INVALID_ARGUMENT`, 504 `DEADLINE_EXCEEDED`, ...),
  with `message` in `grpc-message` and the dotted code in a `crocket-error-code` trailer.
  `grpc::error(grpc::Code::AlreadyExists, "user.exists", "...")` picks the code exactly.
  Extractor failures, limits and deadlines map the same way, and an unknown method is
  `UNIMPLEMENTED`.
- **Deadlines:** a `grpc-timeout` header can shorten `request_timeout` but not extend it.
- **Metadata:** custom metadata arrives as request headers, so `Header<"x-...">` reads it.

gRPC needs HTTP/2. Over TLS it is negotiated with ALPN. A gRPC client on an insecure
channel speaks h2 without TLS, which `LaunchOptions::h2_prior_knowledge` serves. The
same port keeps serving HTTP/1.1.

The `.proto` for clients in other languages comes from the same declarations:

```cpp
std::fputs(grpc::proto_file<^^greeter>("helloworld").c_str(), stdout);
```

It prints a `service Greeter` block and every message and enum the methods use. Enum
values get their enum's prefix, as proto3 requires (`Mood::grumpy` is `MOOD_GRUMPY`), and
a missing zero value is added as `<ENUM>_UNSPECIFIED`. `examples/grpc.cpp` is a complete
service: `crocket_grpc --proto` prints its schema, and grpcurl can call it.

To test without sockets, `LocalClient::grpc` sends one framed message and
`grpc::read_reply<T>` decodes the answer:

```cpp
auto res = client.grpc("/helloworld.Greeter/SayHello", proto::encode(HelloRequest{.name = "Ada"})).dispatch();
assert(grpc::read_reply<HelloReply>(res)->message == "Hello, Ada");
assert(grpc::status_of(client.grpc("/helloworld.Greeter/Nope", "").dispatch()).code == grpc::Code::Unimplemented);
```

### Testing

`LocalClient` dispatches requests through the full pipeline without sockets: fairings,
routing, extractors, responders and error mapping. It plays the role of Rocket's
`local::blocking::Client`.

```cpp
Crocket app = make_app();
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

return crocket::build(cfg)
    .manage(std::move(pool))
    .attach(Logger{})
    .attach(Metrics{})
    .mount("/api", reflect_routes<^^api>())
    .launch({.port = 8443, .tls_cert = "cert.pem", .tls_key = "key.pem"});   // h2 via ALPN
```

What you get:

- **Protocols:** HTTP/1.1, HTTPS, and HTTP/2 when the client negotiates it over TLS.
  Unary gRPC over HTTP/2, either with TLS or on a plain-text port with
  `h2_prior_knowledge = true`.
- **Built-in routes:** `GET /healthz` and `GET /readyz`.
- **Request ids:** an `x-request-id` on every response, honouring the client's value
  when valid.
- **Graceful shutdown on SIGINT/SIGTERM:** stop accepting, drain in-flight requests,
  run `on_shutdown`, then destroy managed state.

Network I/O runs on one event loop per physical core (`LaunchOptions::event_loops`), each
with its own listening socket on the port. Hyperthreads of a core don't get a loop each:
two loops sharing a core compete for it, and a loop that waits for a CPU stalls every
connection it owns. CPUs outside the process's affinity mask (a container's cpuset,
`taskset`) are not counted. `Task<T>` handlers run on the loops, and plain functions
on a pool of `LaunchOptions::workers` threads, or on the loops once they have proved fast
(see [adaptive placement](#adaptive-placement)).

Requests whose headers exceed `LaunchOptions::max_header_bytes` (8 KiB) or
`max_header_count` (100) get 431 `headers.too_large`. The threading model and the other
limits are in [docs/ENGINE.md](docs/ENGINE.md).

### Behind a proxy

The `Client` extractor describes the client: its address, and whether it connected with
`https` or `http`. Behind a load balancer, list the proxies you run so crocket takes both
from the headers they add:

```cpp
Config cfg;
cfg.trusted_proxies = {"10.0.0.0/8"};             // addresses or CIDR ranges
cfg.proxy_header = ProxyHeader::XForwardedFor;    // the default; or ProxyHeader::Forwarded (RFC 7239)
cfg.allowed_hosts = {"api.example.com", "*.example.com"};

[[= http::get("/whoami")]]
auto whoami(Client client) -> std::string { return client.addr + " " + std::string(client.scheme); }
```

- **Only trusted proxies are believed.** A request from any other address keeps its
  own address and scheme, whatever headers it sends. Behind a chain of trusted proxies,
  the client is the first address that no trusted proxy is behind, so a value the
  client wrote into `X-Forwarded-For` itself is never picked.
- **Only the configured header is read.** Set `proxy_header` to the one your proxies
  write. The other is ignored, so a client cannot supply it.
- **`allowed_hosts`** lists the host names the server answers to. Any other `Host`
  (`:authority` in HTTP/2) is 400 `host.invalid` before routing, which stops
  host-header injection in links and redirects. Ports are not compared, and
  `*.example.com` matches subdomains but not `example.com` itself. An empty list
  allows every host.
- Malformed entries in either list fail ignite.

In tests, `LocalClient` sets the peer with `.remote()`:

```cpp
auto r = client.get("/whoami").remote("10.0.0.5").header("x-forwarded-for", "203.0.113.9").dispatch();
```

### Dev profile

`Config::dev()` is for running on your own machine. Choose it in code, or read
`CROCKET_PROFILE` with `Config::from_env()`, which gives `dev()` for `dev`, the release
defaults when unset or `release`, and throws `std::invalid_argument` for anything else:

```cpp
return crocket::build(Config::from_env()) // CROCKET_PROFILE=dev ./my_app
    .attach(Logger{})
    .mount("/", reflect_routes<^^api>())
    .launch({.port = 8000});
```

Compared with the release defaults, the dev profile:

- **Error bodies carry `debug`** (`ApiError::detail`), including an uncaught exception's `what()`, so you
  see the cause in the client without reading logs. Release never sends it.
- **Logs are readable lines**, coloured on a terminal unless `NO_COLOR` is set, and
  `debug` lines are on: `14:02:11.504 GET /hello/Ada/400 404 0.21ms api::hello path.invalid: …`.
- **`launch()` prints every route** before it starts.
- **`GET /__routes` is on**, the request deadline is an hour (room for a debugger
  breakpoint), and the drain on shutdown is one second.
- **An empty `LaunchOptions::host` binds `127.0.0.1`** instead of `0.0.0.0`.
- **`allowed_hosts` also accepts `localhost`, `127.0.0.1` and `::1`**, so a server
  configured for its public name still answers locally.

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
| Error catchers (`#[catch]`) | `ApiError` plus an `on_response` fairing, as in `NotFoundPage` above |
| `on_liftoff` | Not implemented |
| Config profiles (`Rocket.toml` / Figment) | A release and a [dev profile](#dev-profile) chosen with `CROCKET_PROFILE`; other settings are plain structs, read from env or files yourself (see `examples/serve.cpp`) |
| Mutual TLS | Not implemented |

## Building

The toolchain used for development is GCC 16.2 (`-std=c++26 -freflection`, added
automatically as a `PUBLIC` compile option of the `crocket` target), CMake 4.2 and
Ninja on Ubuntu 26.04. You also need OpenSSL and zlib development headers and `perl`
for h2o (`./dev setup` installs them).

GCC 16.2 or later is required. Ubuntu 26.04's `g++-16` package is a pre-release
snapshot (`16-20260322`) whose reflection bugs break any translation unit that mounts
routes. Its errors start with `accessing uninitialized member 'crocket::detail::Binding::kind'`.

CMake links with lld when it can, then mold, then the default linker. Each candidate
is tried with a real test link, because mold 2.40 cannot link with GCC 16.

h2o is pinned to a commit in `cmake/h2o.cmake`, because it has had no release tag since
2019. `./dev h2o-update` moves the pin to the newest h2o and runs the full test suite;
see [docs/ENGINE.md](docs/ENGINE.md#updating-h2o). An application that links with
`-Wl,--gc-sections` drops the parts of h2o crocket never calls, about 300 KB.

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

`./dev help` lists every command. Builds go to `build-dev/` (Debug) and `build/`
(release), configured from `CMakePresets.json`. `./dev` picks the compiler in this
order: `$CROCKET_CXX`, GCC 16.2 in `~/.local/gcc-16.2`, then `g++-16`.

### Benchmarks

```bash
./dev bench --save main          # on main: load and micro benchmarks, kept as a baseline
./dev bench --compare main       # on a branch: the change from main; exits 1 if >10% worse
./dev bench micro                # in-process only (seconds)
./dev bench load router wait     # chosen load scenarios
./dev bench --vs-rocket load     # the same scenarios against Rocket 0.5 too (needs cargo)
```

`load` runs [h2load](https://nghttp2.org/documentation/h2load-howto.html) against
`crocket_bench` ([bench/server.cpp](bench/server.cpp)) over HTTP/1.1 and h2c, and reports
requests per second, p50 and p99 latency, non-2xx responses, and the server's CPU time per
request (`server_us_per_req`: user plus kernel, all threads):

| Scenario | Request | Measures |
|---|---|---|
| `plaintext` | `GET /plaintext` | The pipeline with the least work |
| `json` | `GET /json` | Encoding a small struct |
| `orders` | `POST /orders` | Decoding and encoding a 1 KB order |
| `router` | `GET /api/v39/users/7/orders/9` | A route matched last among about 200 |
| `wait` | `GET /wait` | A handler that blocks its worker for 20 ms |
| `wait_async` | `GET /wait_async` | A handler that suspends for 20 ms without holding a worker |
| `plaintext_async`, `json_async`, `orders_async`, `router_async` | the same under `/async` | The same handlers as `Task<T>`, which run on the event loop with no handoff |

`micro` ([bench/micro.cpp](bench/micro.cpp)) times routing, the in-process request
pipeline and JSON, and counts heap allocations per operation. Results are saved in
`bench/results/`, which git ignores: numbers compare only on the same machine, with the
same load on it. The load generator shares the machine with the server, so treat
differences of a few percent as noise.

When h2load itself runs out of CPU, requests per second measures the client, not the
server, and `./dev bench` warns: `h2load's threads were 98% busy, so req/s is the client's
limit`. CPU per request does not depend on the client, so compare that instead. `--duration`, `--clients` and `--threads` (h2load's) change the load.

`--vs-rocket` builds [bench/rocket](bench/rocket) (Rocket 0.5, the same routes and bodies)
with cargo, runs the same scenarios against it over HTTP/1.1 (Rocket serves HTTP/2 only
over TLS), and prints the two side by side. The load is closed-loop: each connection waits
for its response before sending the next request. So with few connections the numbers
mostly reflect per-request latency, and with many (`--clients 256` or more) the server's
throughput, until the client runs out of CPU. The last column, CPU per request, compares
the two servers whatever the client's limit.

## Tests

| Test | What it checks |
|---|---|
| `acceptance` | The request pipeline in-process via `LocalClient`, with no sockets. Covers acceptance items 1–4 and 6, plus routing, responders, request ids, log lines, application logging (templates, context, redaction, sampling, levels, a full buffer), metrics, health checks, CORS, pools, the dev profile, header validation, trusted proxies, allowed hosts, `Shield`, ETags, 304s and ranges. |
| `json` | JSON in-process: round trips, field paths, the 64-bit range and `as_string`, UTF-8 checking and repair, duplicate keys, limits, every annotation, chrono, variants, validation, the regex engine (including inputs that make backtracking engines hang) and problem+json bodies from `Json<T>`. |
| `cpus` | The default number of event loops: physical cores from CPU topology, counting hyperthread siblings once, honouring the affinity mask, and falling back when topology is missing. |
| `router` | The route tree against a linear scan of every route, over 2,000 random route tables: the same candidates in the same order, the same captures, HEAD falling back to GET, and the same methods for 405. |
| `async` | `Task<T>` handlers in-process: suspension and resumption, request context after a resume, the hop back from a foreign library's thread, `callback<T>()` (later, now, twice, abandoned), exceptions after a resume, async gRPC methods, and 200 concurrent sleeping handlers. |
| `compile_fail.*` | Programs that must not compile. Covers acceptance item 5, where the `{age}` vs `years` diagnostic must name both identifiers, plus other misuses and a control file that must compile. |
| `consumer` | `tests/consumer`, a small application that adds crocket with FetchContent, builds with only `crocket::crocket` linked (no C++ standard of its own), and serves one request. It also fails if crocket's targets or h2o's options leak into the application's cache. |
| `tls_h2` | Real sockets, self-signed TLS, ALPN h2 and http/1.1, and the handler suite over both, including custom headers, `X-Request-Id`, CORS preflight, 431 for oversized headers and too many headers, a 304 for a matching `If-None-Match`, security headers, 20 async handlers sharing 2 workers, and a deadline waking a sleeping task. Also adaptive placement: a route promoted after 1,000 fast runs, demoted by one slow run, and a `Pool` route that is never promoted. Also h2 bodies larger than the flow-control window, chunked uploads, a 413 that keeps the h1 connection, a handler longer than the h2 idle timeout, and graceful drain (acceptance item 7). Needs `openssl` and a curl built with HTTP/2. Uses ports 18443 and 8000. |
| `grpc` | Protobuf encoding byte for byte, decode errors, unary calls, status mapping, the generated `.proto` and mount validation, in-process. |
| `grpc_h2` | `crocket_grpc` over real sockets, driven by curl: h2 with prior knowledge and over TLS, trailers, trailers-only errors, custom metadata, HTTP/1.1 on the h2 port, and 300 KB requests and replies with and without `Content-Length`. Uses ports 18551 and 18552. |

## `crocket_serve` configuration

`crocket_serve` reads its settings from environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `CROCKET_PROFILE` | `release` | `dev` for the [dev profile](#dev-profile) |
| `CROCKET_HOST` | `0.0.0.0` (`127.0.0.1` in dev) | Address to listen on |
| `CROCKET_PORT` | `8000` | Port to listen on |
| `CROCKET_TLS_CERT`, `CROCKET_TLS_KEY` | unset | Enable TLS; both must be set |
| `CROCKET_REQUEST_TIMEOUT` | `10` (an hour in dev) | Request deadline in seconds |
| `CROCKET_DEBUG_ROUTES` | unset | Set to any value to expose `GET /__routes` |
| `CROCKET_WORKERS` | one per core | Handler threads |
| `CROCKET_EVENT_LOOPS` | one per physical core | Network I/O threads |

## Differences from REQUIREMENTS.md

- A missing bearer token is 401 `auth.missing`, and a non-Bearer scheme is 401
  `auth.invalid`. The requirements list `auth.expired`; the `Authenticator` still
  returns `auth.expired` for expired tokens.
- `Json<T>` with a non-JSON `content-type` is 415 `json.unsupported_media_type`.
  Malformed JSON is 422 `json.invalid` as specified. Validation failures, unknown fields
  under `deny_unknown_fields`, duplicate keys and exceeded limits have their own codes
  (see [JSON](#json)).
- Error bodies are RFC 9457 problem details (`application/problem+json`). `message` is
  sent as `detail`, and `ApiError::detail` stays in the logs.

## License

crocket is released under the [MIT license](LICENSE).

It links [h2o](https://github.com/h2o/h2o) statically into your program. h2o is
MIT-licensed, and so are the libraries it bundles into it (picotls, quicly,
picohttpparser, hiredis, libyrmcds, libgkc and cloexec). A binary you distribute must
include their notices: `LICENSE` in the h2o source, and each library's license under
its `deps/` directory. OpenSSL is linked dynamically under the Apache License 2.0, and
zlib under the zlib license.
