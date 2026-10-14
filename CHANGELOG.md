# Changelog

All notable changes to crocket. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and crocket uses [Semantic Versioning](https://semver.org/). While on 0.x, a minor
bump (0.1 → 0.2) may break the API; patch releases are fixes and additions only.

Releases are cut by pushing a `X.Y.Z` tag; the release workflow copies the matching
section below into the GitHub Release.

## [Unreleased]

### Added
- **`crocket::db`**, an optional component for Postgres on libpqxx (built when libpqxx
  is found; `-DCROCKET_DB=OFF|ON`). Plain structs, no annotations or base class: the
  table is the type's name in plural snake_case (`Hero` -> `heroes`, overridable with
  `db::table_name<T>`), columns are the members, `id` is the key. Handlers take `Db& db`;
  `db.find<T>(id)`, `db.insert(v)`, `db.update(v)`, `db.erase<T>(id)`,
  `db.where(&T::member, value | db::like(...) | db::gt(...) | db::in({...}))` with
  `order_by`, `limit`, `offset`, `all`, `first` and `count`, plus `db.sql<T>(...)` and
  `db.exec(...)` for raw SQL. Statements are prepared once per connection.
- **`crocket_db_seconds{table,op}`** and **`crocket_db_pool_connections{state}`** in
  `Metrics`, and **`db_ms`, `db_wait_ms` and `db_queries`** on `Logger` request lines,
  when a request used the database.
- **`Config::database`** (`url`, `pool_size`, `pool_wait`) and `CROCKET_DATABASE_URL`,
  `CROCKET_DATABASE_POOL_SIZE` and `CROCKET_DATABASE_POOL_WAIT` in `Config::from_env()`.
- `examples/heroes.cpp`: a CRUD API on `crocket::db`, and `bench/db.cpp`, `crocket::db`
  against the same queries written with libpqxx.

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
  and which of them the environment reached.
- **`launch()` serves on `cfg.server`**; `launch({...})` takes `ServerOverrides`, which
  overrides only the server fields you name: `.launch({.listen = "8080"})`.
  `listen` accepts `"8080"`, `":8080"`, `"host:8080"` and `"[::1]:8080"`.
- **`Config::from_env()` reads every `CROCKET_*` setting**, not just `CROCKET_PROFILE`,
  with strict parsing: a bad value throws `std::invalid_argument` naming the variable
  (`CROCKET_PORT=tcp://10.43.0.12:8000` is rejected instead of misread).
  `Config::from_env(base)` layers the profile and environment over your own defaults.
  Precedence: defaults < base < profile < environment < code < `launch({...})`.
- `CROCKET_REQUEST_TIMEOUT` accepts units (`500ms`, `30s`, `2m`); a bare number is still
  seconds. `CROCKET_DEBUG_ROUTES` is a boolean now: `0`/`false` turns it off.
- `examples/serve.cpp`, `grpc.cpp` and `hello.cpp` use `Config::from_env()` and
  `launch()` instead of mapping variables by hand.

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
  `:experimental` for `X.Y.Z-rc.N` pre-releases (#31).
- `crocket_build_info{version,commit,compiler,hostname} 1` in `Metrics` (#31).
- Constant labels on every metrics series via `CROCKET_METRICS_LABELS`
  (for example `pod=$(POD_NAME)`), or `Metrics{path, {{"pod", ...}}}` (#31).
- GCC 16.2 toolchain image (`ghcr.io/sean3z/crocket-toolchain:gcc-16.2`) for CI and dev containers.
- `crocket-hello` image, a Compiler Explorer example and a "Try it" section in the README.
- `docs/DESIGN.md`, describing crocket as it is.

### Changed
- CI runs in the toolchain image instead of building GCC (about 29 minutes down to 3).
- `-freflection` is added only for GCC, and configure fails early with a clear message
  when the compiler lacks C++26 reflection (#20, #21, #28).
- README: FetchContent pins a release tag (#22, #29).

### Fixed
- Metrics: 504 deadline responses are labelled with their matched route instead of
  `<unmatched>` (#30).

[Unreleased]: https://github.com/sean3z/crocket/compare/0.2.0...HEAD
[0.2.0]: https://github.com/sean3z/crocket/compare/0.1.1...0.2.0
[0.1.1]: https://github.com/sean3z/crocket/releases/tag/0.1.1
