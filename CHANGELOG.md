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
  `8c95f068`, RFC 9112).

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

[Unreleased]: https://github.com/sean3z/crocket/compare/0.4.0...HEAD
[0.4.0]: https://github.com/sean3z/crocket/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/sean3z/crocket/compare/0.2.0...0.3.0
[0.2.0]: https://github.com/sean3z/crocket/compare/0.1.1...0.2.0
[0.1.1]: https://github.com/sean3z/crocket/releases/tag/0.1.1
