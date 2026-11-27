# Changelog

All notable changes to crocket. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and crocket uses [Semantic Versioning](https://semver.org/). While on 0.x, a minor
bump (0.1 → 0.2) may break the API; patch releases are fixes and additions only.

Releases are cut by pushing a `X.Y.Z` tag; the release workflow copies the matching
section below into the GitHub Release.

Every entry links its pull request and any issues it closes, and credits contributors
from outside the project, for example
`([#29](https://github.com/sean3z/crocket/pull/29), by @Wkhanex, closes [#22](https://github.com/sean3z/crocket/issues/22))`.

## [Unreleased]

## [0.5.3] - 2026-11-27

SIGTERM tears a drained loop down on the thread that owns its sockets, an
oversized HTTP/1 body is a 413 the client can read, the toolchain image ships a
static GCC 16.2 libpqxx 7.10, and `crocket_saturation` drops to 0 when a
dependency is saturated.

### Added
- **Dependency-aware saturation.** `Metrics` now reports `crocket_pod_saturation` and
  `crocket_dependency_saturation` (both 0..1). `crocket_saturation`, the HPA target, is
  pod saturation times one minus dependency saturation, so it drops to 0 when the
  database or a downstream dependency is saturated. A slow or failing database, or
  an upstream that timed out, no longer looks like a reason to add pods. With no
  dependency pressure the gauge equals pod saturation, so a service without a database
  scales as it did before. See the README section "Autoscaling on crocket_saturation"
  ([#62](https://github.com/sean3z/crocket/pull/62)).

### Fixed
- **Graceful shutdown.** On SIGTERM each drained event loop, on its own thread, runs
  one more turn so sockets it closed are freed there, disposes its h2o context
  (serialized across loops, which share the proxy socket-pool timer), and clears
  every h2o recycler with `h2o_cleanup_thread`. Those pools are thread-local. The
  loop used to clear only the buffer pool, and the main thread then destroyed the
  context and freed sockets the loop had only queued. `on_shutdown` still runs after
  the loops are torn down, then managed state is destroyed
  ([#60](https://github.com/sean3z/crocket/pull/60)).
- **Oversized HTTP/1 bodies.** A `Content-Length` above `max_body_bytes` already
  answered `413` `body.too_large` before the body was stored. HTTP/1 then read and
  dropped at most 8 MiB and closed the socket. Unread bytes still in the kernel
  receive buffer make that close a TCP RST, so a client that writes the whole body
  before it reads never sees the 413 (about 16 MiB and up). HTTP/1 now keeps pulling
  and dropping until the body ends. The bytes are not stored. HTTP/2 is unchanged
  (`RST_STREAM` with `NO_ERROR` after the response)
  ([#60](https://github.com/sean3z/crocket/pull/60)).
- **Toolchain image.** `ghcr.io/sean3z/crocket-toolchain:gcc-16.2` and `:gcc-16.2-pg`
  are the same image, and a downstream app builds in it without extra compiler
  flags or packages. `gcc`/`cc` and `CC`/`CXX` are GCC 16.2, so CMake finds a C
  compiler. The image installs h2o's build dependencies (OpenSSL, zlib,
  pkg-config) and a static libpqxx 7.10 built with that GCC in C++26. Ubuntu's
  shared libpqxx, built with the system GCC, aborted on exit inside
  `__cxa_finalize` (`double free or corruption`, exit 139) when linked into a
  GCC 16.2 binary. CMake binaries get an rpath to `/opt/gcc-16.2/lib64`, and that
  directory is on the loader path, so the binary runs in the image with no
  `LD_LIBRARY_PATH`. A runtime image copies `/opt/gcc-16.2/runtime/`
  (`libstdc++.so.6` and `libgcc_s.so.1`) onto that path and installs OpenSSL,
  zlib, and libpq. See the deployment section
  ([#61](https://github.com/sean3z/crocket/pull/61)).

## [0.5.2] - 2026-11-23

Static file serving with `.mount(prefix, files{...})`, from a directory or `#embed` bytes, plus `Teapot<T>` for HTTP 418 I'm a Teapot.

### Added
- **`Teapot<T>`** for HTTP 418 I'm a Teapot. RFC 9110 reserves the code, and the reason phrase comes from RFC 2324. It sits with `Created` and `Accepted`, so the status line, problem titles, and OpenAPI descriptions all say I'm a Teapot
  ([#56](https://github.com/sean3z/crocket/pull/56)).
- **Static files.** `.mount(prefix, files{...})` serves a directory under that
  prefix, or bytes embedded with `#embed`. Routes match before files, whichever
  was mounted first, and a miss is a normal 404 unless `.fallthrough`. `.cache`
  and `.headers` pick `Cache-Control` and extra response headers by glob
  (these headers replace Shield's value for the same name). `.precompressed`
  serves a sibling `.br` or `.gz` when `Accept-Encoding` allows it. GET and HEAD
  send a content type
  (text types include a charset), a strong ETag, `Cache-Control`, and
  `Last-Modified` for disk files. `If-None-Match` and `If-Modified-Since`
  answer 304, and one byte range answers 206, through the same conditional
  handling as any other response. Dotfiles are hidden unless asked for.
  Symlinks are followed only when the target stays inside the root. A segment
  is decoded the way a path capture is, and `%2e%2e%2f` (`../`) is a miss
  rather than a path joined onto the root. `..`, encoded slashes, empty
  segments, and NUL never leave the root. An
  optional `spa` flag serves the index for extension-less misses. Disk reads
  run on workers. Outside the dev profile, `If-None-Match` and
  `If-Modified-Since` are answered from size and mtime after `fstat`, without
  reading the body or opening a precompressed sibling, and `HEAD` sends
  `Content-Length` with no body. `.memory` (on by default; `memory_file` 256
  KiB, `memory_bytes` 8 MiB, `memory_revalidate` 1s) keeps those small files
  in memory, keyed by relative path and encoding. A hit inside the revalidate
  window is served on the event loop with no syscall; after it, one `fstat`
  compares size and mtime and drops the entry when the file changed or was
  deleted. The window is how long a replaced or deleted file can still be
  served. inotify isn't used: it misses an atomic replace and doesn't work on
  NFS. The dev profile ignores the cache and the early 304 and reads the disk
  on every request. `HEAD` never sends a body: outside dev, `Content-Length`
  comes from the stat, and in dev the file is still read so `Content-Length`,
  `ETag`, and `Last-Modified` match that read, then the body is dropped.
  Embedded files and the SPA index do the same. `examples/files.cpp` is
  `crocket_files`
  ([#55](https://github.com/sean3z/crocket/pull/55)).

## [0.5.1] - 2026-11-17

`0.5.0` was tagged by mistake and then removed, so `0.5.1` is the first 0.5 release. There is no `0.5.0` to use.

Optional JWT/OIDC and an outbound HTTP client, credential redaction in logs,
and continuous fuzzing. Apps must rebuild: `Config` gained `auth` and `Request`
gained bearer material. `crocket::auth` links `crocket::client`.
`CROCKET_AUTH=ON` fails configure when the client is off, and with both left
on `AUTO`, turning the client off turns auth off. Log redaction now treats `-`
and `_` as the same character, so a field such as `X-Api-Key` matches `api_key`.

### Added
- **Style guide** for new and touched code: [docs/style.md](docs/style.md). It records the conventions already in the tree. clang-format stays off
  ([#52](https://github.com/sean3z/crocket/pull/52)).
- **Continuous fuzzing.** [`.github/workflows/fuzz.yml`](.github/workflows/fuzz.yml)
  runs `./dev fuzz` with AddressSanitizer once a day and on demand, apart from
  pull-request CI (which still runs each corpus and two seconds of mutations).
  Each target is capped at 90 seconds and the job at 30 minutes. A crash or hang
  fails the job and uploads the input, seed, and log. Nothing is committed back
  into `tests/fuzz/corpus`; copy a crash there by hand so it stays a regression
  ([#49](https://github.com/sean3z/crocket/pull/49)).
- **`crocket::client`:** opt-in outbound HTTP on h2o's client, same event loop.
  `http::Client` carries a base URL, timeout, per-host pool, retries, and a
  circuit breaker. Retries and the breaker are on unless a call opts out
  (`{.retry = retry::none}`, `{.breaker = breaker::none}`). Only idempotent
  methods are retried, plus any call that sets `Idempotency-Key`, and a retry
  budget caps retries at 20% of recent calls (at least 3). Failures are
  `upstream.timeout` (504), `upstream.unavailable` (503), and
  `upstream.circuit_open` (503). An inbound `x-request-id` is forwarded;
  `traceparent` is not. `examples/client.cpp` is `crocket_client_example`.
  Two services are two clients: `.with_state<Tag>()` and
  `State<http::Client::For<Tag>>` (`examples/client_multi.cpp`,
  `crocket_client_multi_example`). `./dev bench client` times one localhost GET
  ([#46](https://github.com/sean3z/crocket/pull/46)).
- **`crocket::auth`:** opt-in JWT and OIDC (`auth::Jwt`, `Auth<T>`, `auth::require` /
  `auth::any_of`). Defaults are RS256, ES256 and EdDSA. HS256 (and HS384/HS512)
  verify only with an explicit `.secret` / `CROCKET_AUTH_SECRET`, and are not in
  the default algorithm list. JWKS refresh does not block a request; `/readyz`
  stays not ready until the first key set loads. `examples/auth.cpp` is
  `crocket_auth_example` when auth is on, and `./dev bench auth` times
  `me(Auth<>)`. See the README
  ([#45](https://github.com/sean3z/crocket/pull/45))

### Changed
- **Docs.** CI is `./dev check` in the GCC 16.2 toolchain image, not a compiler
  build in a plain Ubuntu container. The CMake options for the client, auth, and
  Postgres say `AUTO` / `ON` / `OFF`, including auth failing closed when the
  client is off. Logging states the request-line credential scrub next to
  field-name redaction. `db::Pool` throws if it cannot open its connections at
  startup. Versions note that 1.0 is not released and that 0.x minors have no
  compatibility shim. The roadmap section points at the 1.0 roadmap issue.
  ([#50](https://github.com/sean3z/crocket/pull/50)).
- **Log redaction** treats `-` and `_` as the same character, so a field named
  `X-Api-Key` matches `api_key`. `http::for_log` prints headers, a call, a
  response, or `ClientOptions` without userinfo, query, fragment, CA paths, or
  header values whose names look like credentials. Bodies are a byte count.
  Logging a raw string under an unrelated field name is still printed as
  itself. The bytes on the wire are unchanged
  ([#48](https://github.com/sean3z/crocket/pull/48)).
- **`crocket::auth` JWKS fetch** uses `crocket::client` instead of its own HTTPS
  client. A fetch times out at 5 seconds, keeps at most 1 MiB, checks the TLS
  hostname, and does not retry; the refresh loop tries again. `crocket::auth`
  links `crocket::client`. `CROCKET_AUTH=ON` with the client off fails configure.
  With both on `AUTO`, turning the client off turns auth off
  ([#47](https://github.com/sean3z/crocket/pull/47)).

### Fixed
- **Adaptive placement in `tls_h2`.** The socket test required 1,000 handler
  runs in a row under 100 µs of wall time. One preemption on a busy runner
  resets that streak, so the route stayed on workers and the demotion checks
  failed with it. The same thresholds are now applied to durations the test
  supplies (`tests/placement.cpp`). The socket test still checks that a plain
  route starts on workers and that a `Pool` route stays there
  ([#51](https://github.com/sean3z/crocket/pull/51)).

### Security
- **Vulnerability reports:** [SECURITY.md](SECURITY.md) is the security policy.
  It says how to report a vulnerability in private, what is in scope for the
  framework, and how long a report should expect to wait
  ([#43](https://github.com/sean3z/crocket/pull/43))
- **Signed hello images and SBOMs:** the release workflow signs
  `ghcr.io/sean3z/crocket-hello` with Sigstore (keyless) on every channel, and
  publishes a CycloneDX and an SPDX software bill of materials for that image.
  A tagged release attaches both files. [SECURITY.md](SECURITY.md) has the
  verify commands
  ([#43](https://github.com/sean3z/crocket/pull/43))

## [0.4.0] - 2026-10-25

Throughput, logging, and autoscaling. Roughly 1.6x h2c with production features
on, workers that follow the database pool, and `crocket_saturation` for HPA or
KEDA. Apps must rebuild (a header type gained a field).

### Added
- **Autoscaling metrics:** `Metrics` now reports `crocket_worker_busy_seconds_total`,
  `crocket_worker_busy_ratio`, a `crocket_request_queue_wait_seconds` histogram, a
  `crocket_event_loop_lag_seconds` histogram, `crocket_requests_shed_total{reason}`
  and, with `crocket::db`, `crocket_db_pool_size` and a `crocket_db_pool_wait_seconds`
  histogram. `crocket_saturation` (0..1) combines them for an HPA or KEDA target; the
  README documents the formula and example configs ([#41](https://github.com/sean3z/crocket/pull/41))
- **Environment variables for existing log settings:** `CROCKET_LOG_SAMPLE` sets
  `log.sample` and `CROCKET_LOG_BUFFER` sets `log.buffer` (both options already
  existed in code) ([#39](https://github.com/sean3z/crocket/pull/39))
- **Slow-request logging:** new `log::Options::slow` (`CROCKET_LOG_SLOW`, default
  `500ms`). With `sample` below 1, requests at least this slow are always logged;
  `0` turns that off ([#39](https://github.com/sean3z/crocket/pull/39))

### Changed
- **Workers follow the database pool:** when the worker count isn't set
  (`CROCKET_WORKERS`, `server.workers` or `launch({.workers})`), crocket now starts at
  least as many workers as the largest managed blocking pool (`db::Pool` or `Pool<T>`)
  has connections. Before, a 16-connection pool on 4 cores ran only 4 queries at a time
  and capped a Postgres-backed Heroes API at about 11k req/s on a 4-vCPU server, against
  31k with 16 workers. An explicit worker count still wins
  ([#40](https://github.com/sean3z/crocket/pull/40))
- **Faster hot path:** pooled header storage (fewer allocations), lock-free metrics and
  log shards, one clock read per request, and batched `writev` log writes with a bigger
  pipe. Roughly 1.6x h2c throughput with every production feature on, and far fewer
  dropped log lines. No API changes, but apps must rebuild (the header type gained a
  field). Request log `ts`/`duration_ms` mark when the handler returned, about 1 µs
  earlier ([#39](https://github.com/sean3z/crocket/pull/39))

### Fixed
- **libpqxx 8:** `crocket::pg` builds and runs against libpqxx 8.x as well as 7.10.
  Rows are decoded from `pqxx::row_ref`, and `db::in(...)` builds its array literal
  itself, because libpqxx 8 won't convert an array without the connection's encoding.
  CI now also builds and runs the db test against libpqxx 8.0
  ([#40](https://github.com/sean3z/crocket/pull/40))

### Security
- **h2o** is pinned to master `8c95f068` (2026-10-24), past the 2026-10-23
  security fixes. That includes `3d47065`, which removes HTTP/2 Cache-Digest
  decoding and closes
  [GHSA-48jv-5969-gjg3](https://github.com/h2o/h2o/security/advisories/GHSA-48jv-5969-gjg3)
  (high, CVSS 7.5): an unauthenticated client can send Cache-Digest headers that
  expand until the process runs out of memory. The same pin includes the TLS 1.3
  upstream hostname check
  ([GHSA-67m7-gf4w-hw23](https://github.com/h2o/h2o/security/advisories/GHSA-67m7-gf4w-hw23));
  crocket does not use h2o as a reverse proxy. A request that sends both
  `Content-Length` and `Transfer-Encoding` now closes the connection (h2o
  `8c95f068`, RFC 9112)
  ([#42](https://github.com/sean3z/crocket/pull/42))

## [0.3.0] - 2026-10-14

Databases: `crocket::db` with its Postgres backend `crocket::pg`, and handlers that
return plain structs. Nothing existing breaks; the new pieces are opt-in.

### Added
- **`crocket::db`**, a small database API, with its first backend **`crocket::pg`**
  (CMake target), Postgres on libpqxx, built when libpqxx is found
  (`-DCROCKET_PG=AUTO|ON|OFF`). Handlers take `Db&` and never name the backend. Plain structs, no annotations or base class: the
  table is the type's name in plural snake_case (`Hero` -> `heroes`, overridable with
  `db::table_name<T>`), columns are the members, `id` is the key. Handlers take `Db& db`;
  `db.find<T>(id)`, `db.insert(v)`, `db.update(v)`, `db.erase<T>(id)`,
  `db.where(&T::member, value | db::like(...) | db::gt(...) | db::in({...}))` with
  `order_by`, `limit`, `offset`, `all`, `first` and `count`, plus `db.sql<T>(...)` and
  `db.exec(...)` for raw SQL. Statements are prepared once per connection ([#34](https://github.com/sean3z/crocket/pull/34)).
- A model's `id` may be missing from a JSON body (it reads as 0 and the database
  assigns it); `insert()` never sends `id`, so a client-supplied one is ignored.
  `json::omittable<T>` is the hook ([#34](https://github.com/sean3z/crocket/pull/34)).
- **`crocket_db_seconds{table,op}`** and **`crocket_db_pool_connections{state}`** in
  `Metrics`, and **`db_ms`, `db_wait_ms` and `db_queries`** on `Logger` request lines,
  when a request used the database ([#34](https://github.com/sean3z/crocket/pull/34)).
- **`Config::database`** (`url`, `pool_size`, `pool_wait`) and `CROCKET_DATABASE_URL`,
  `CROCKET_DATABASE_POOL_SIZE` and `CROCKET_DATABASE_POOL_WAIT` in `Config::from_env()` ([#34](https://github.com/sean3z/crocket/pull/34)).
- **Handlers can return a plain struct**: `Hero`, `std::vector<Hero>`, `std::optional<Hero>`
  (empty: 404) and `Created<Hero>` are JSON on HTTP routes, as if wrapped in `Json<T>`,
  and protobuf on gRPC routes. `Json<T>` still works and always wins ([#34](https://github.com/sean3z/crocket/pull/34)).
- **`Proto<T>`**: an explicit protobuf body, `application/x-protobuf` on HTTP routes and
  the normal reply on gRPC routes. A gRPC method returning `Json<T>` replies with JSON
  (`application/grpc+json`). OpenAPI lists `application/x-protobuf` for `Proto<T>` ([#34](https://github.com/sean3z/crocket/pull/34)).
- `examples/heroes.cpp`: a CRUD API on `crocket::db`, and `bench/db.cpp`, `crocket::db`
  against the same queries written with libpqxx ([#34](https://github.com/sean3z/crocket/pull/34)).

## [0.2.0] - 2026-10-13

One `Config` for everything, configurable from the environment. Breaking: settings
moved, so code that sets them needs the new names.

### Changed
- **`LaunchOptions` is gone; `Config` holds every setting**, in groups: `server`
  (`listen`, `host`, `port`, `workers`, `event_loops`, `tls_cert`, `tls_key`, `http2`,
  `h2_prior_knowledge`), `limits` (`max_body_bytes`, `max_body_memory`, `max_in_flight`,
  `max_header_bytes`, `max_header_count`, `max_connections`, `max_connections_per_ip`,
  `http2_max_streams`, `etag_max_bytes`), `timeouts` (`request`, `drain`) and `proxy`
  (`trusted`, `header`, `allowed_hosts`), plus `profile`, `debug_routes`, `json`, `log`
  and `adaptive_placement`. Having two structs made it unclear where a setting lived
  and which of them the environment reached ([#33](https://github.com/sean3z/crocket/pull/33)).
- **`launch()` serves on `cfg.server`**; `launch({...})` takes `ServerOverrides`, which
  overrides only the server fields you name: `.launch({.listen = "8080"})`.
  `listen` accepts `"8080"`, `":8080"`, `"host:8080"` and `"[::1]:8080"` ([#33](https://github.com/sean3z/crocket/pull/33)).
- **`Config::from_env()` reads every `CROCKET_*` setting**, not just `CROCKET_PROFILE`,
  with strict parsing: a bad value throws `std::invalid_argument` naming the variable
  (`CROCKET_PORT=tcp://10.43.0.12:8000` is rejected instead of misread).
  `Config::from_env(base)` layers the profile and environment over your own defaults.
  Precedence: defaults < base < profile < environment < code < `launch({...})` ([#33](https://github.com/sean3z/crocket/pull/33)).
- `CROCKET_REQUEST_TIMEOUT` accepts units (`500ms`, `30s`, `2m`); a bare number is still
  seconds. `CROCKET_DEBUG_ROUTES` is a boolean now: `0`/`false` turns it off ([#33](https://github.com/sean3z/crocket/pull/33)).
- `examples/serve.cpp`, `grpc.cpp` and `hello.cpp` use `Config::from_env()` and
  `launch()` instead of mapping variables by hand ([#33](https://github.com/sean3z/crocket/pull/33)).

### Migrating from 0.1
| 0.1 | 0.2 |
|---|---|
| `cfg.request_timeout`, `cfg.drain_timeout` | `cfg.timeouts.request`, `cfg.timeouts.drain` |
| `cfg.max_body_bytes`, `max_in_flight`, `max_body_memory`, `etag_max_bytes` | `cfg.limits.*` |
| `cfg.trusted_proxies`, `proxy_header`, `allowed_hosts` | `cfg.proxy.trusted`, `.header`, `.allowed_hosts` |
| `LaunchOptions{.max_connections = ...}` and the other limits | `cfg.limits.*` |
| `.launch({.host = ..., .port = ..., .tls_cert = ...})` | unchanged, or `cfg.server.*` and `.launch()` |
| Hand-written `getenv("CROCKET_...")` | `Config::from_env()` |

## [0.1.1] - 2026-10-13

The first tagged release: the stable channel starts here.

### Added
- Versions and release channels: `project(crocket VERSION 0.1.1)`, a generated
  `<crocket/version.hpp>` (`crocket::version::string`, `commit`, `compiler`), and a
  release workflow that publishes `ghcr.io/sean3z/crocket-hello` as `:X.Y.Z`, `:X.Y` and
  `:stable` for release tags, `:latest` and `:sha-<short>` for `main`, and
  `:experimental` for `X.Y.Z-rc.N` pre-releases ([#32](https://github.com/sean3z/crocket/pull/32), implements [#31](https://github.com/sean3z/crocket/issues/31)).
- `crocket_build_info{version,commit,compiler,hostname} 1` in `Metrics` ([#32](https://github.com/sean3z/crocket/pull/32)).
- Constant labels on every metrics series via `CROCKET_METRICS_LABELS`
  (for example `pod=$(POD_NAME)`), or `Metrics{path, {{"pod", ...}}}` ([#32](https://github.com/sean3z/crocket/pull/32)).
- GCC 16.2 toolchain image (`ghcr.io/sean3z/crocket-toolchain:gcc-16.2`) for CI and dev containers ([#24](https://github.com/sean3z/crocket/pull/24)).
- `crocket-hello` image, a Compiler Explorer example and a "Try it" section in the README ([#26](https://github.com/sean3z/crocket/pull/26), [#27](https://github.com/sean3z/crocket/pull/27)).
- `docs/DESIGN.md`, describing crocket as it is.

### Changed
- CI runs in the toolchain image instead of building GCC (about 29 minutes down to 3) ([#25](https://github.com/sean3z/crocket/pull/25)).
- `-freflection` is added only for GCC, and configure fails early with a clear message
  when the compiler lacks C++26 reflection ([#28](https://github.com/sean3z/crocket/pull/28), fixes [#20](https://github.com/sean3z/crocket/issues/20) and [#21](https://github.com/sean3z/crocket/issues/21)).
- README: FetchContent pins a commit instead of `main` ([#29](https://github.com/sean3z/crocket/pull/29), by @Wkhanex, closes [#22](https://github.com/sean3z/crocket/issues/22)), then
  the release tag ([#32](https://github.com/sean3z/crocket/pull/32)).

### Fixed
- Metrics: 504 deadline responses are labeled with their matched route instead of
  `<unmatched>` ([#30](https://github.com/sean3z/crocket/pull/30)).

[Unreleased]: https://github.com/sean3z/crocket/compare/0.5.3...HEAD
[0.5.3]: https://github.com/sean3z/crocket/compare/0.5.2...0.5.3
[0.5.2]: https://github.com/sean3z/crocket/compare/0.5.1...0.5.2
[0.5.1]: https://github.com/sean3z/crocket/compare/0.4.0...0.5.1
[0.4.0]: https://github.com/sean3z/crocket/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/sean3z/crocket/compare/0.2.0...0.3.0
[0.2.0]: https://github.com/sean3z/crocket/compare/0.1.1...0.2.0
[0.1.1]: https://github.com/sean3z/crocket/releases/tag/0.1.1
