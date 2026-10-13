# Changelog

All notable changes to crocket. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and crocket uses [Semantic Versioning](https://semver.org/). While on 0.x, a minor
bump (0.1 → 0.2) may break the API; patch releases are fixes and additions only.

Releases are cut by pushing a `X.Y.Z` tag; the release workflow copies the matching
section below into the GitHub Release.

## [Unreleased]

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

[Unreleased]: https://github.com/sean3z/crocket/compare/0.1.1...HEAD
[0.1.1]: https://github.com/sean3z/crocket/releases/tag/0.1.1
