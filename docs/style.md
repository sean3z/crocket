# Style

How crocket is already written. Follow this for new code and for the lines you touch. Don't reformat a file just to match it. Don't rename a public API for style.

There's no `.clang-format` and CI doesn't format. clang-format mangles the GCC 16.2 reflection syntax this tree uses: `[[= ...]]` annotations, `template for`, and `[: ... :]` splices. Format by hand.

## Layout

- Two spaces. No tabs.
- The opening brace stays on the same line.
- A single-statement `if` or `for` has no braces. Use braces when the body is more than one statement.
- `T*` and `T&`, the star and ampersand next to the type. `const` goes to the left: `const T&`.
- A short gloss can sit at the end of a line. A longer note is `///` above the declaration.

## Names

- Functions and variables are `snake_case`. Types are `PascalCase`. Enumerators are `PascalCase` too, except a published token such as `RS256`.
- Private data members end in `_`.
- `crocket::detail` is for internals shared across files. An anonymous namespace is for helpers that stay in one `.cpp`.
- Close a namespace with `}  // namespace crocket` (and the nested name when there is one).
- American English in comments, docs, and identifiers you choose. A protocol name stays as published (`Cancelled` in gRPC).

## Headers and includes

Public headers start with `#pragma once` and a `//` banner that says what the header is for, with a short usage snippet when the API isn't obvious.

Include order, with a blank line between groups:

1. This file's header, or other project headers (`"crocket/..."`, `"engine.hpp"`).
2. Third-party libraries (`<h2o.h>`, `<openssl/...>`, `<pqxx/pqxx>`).
3. C headers (`<unistd.h>`).
4. C++ standard headers, alphabetical. Put `<meta>` first in that group when the file uses reflection.

h2o and OpenSSL stay out of public headers. `crocket.hpp` doesn't include them. `db.hpp` includes libpqxx because that API returns `pqxx` types. Don't copy that exception unless the public API really exposes the library.

Optional pieces follow `crocket::client`, `crocket::auth`, and `crocket::pg`: CMake `AUTO` / `ON` / `OFF`, a separate target, and a header that `crocket.hpp` doesn't include.

## Types and errors

Config, options, and route annotations stay aggregates, so callers can use designated initializers. Don't add a constructor, a base, or a `std::optional` member to one of those.

- An extractor returns `std::expected<T, ApiError>`.
- Config and environment parsing throws `std::invalid_argument` and names the bad setting.
- A `consteval` check throws `std::meta::exception` and passes the reflected construct (`^^Route`). Don't throw a string literal.
- A handler may throw `ApiError`. The pipeline turns that into the error response. `db` does this.

## Tests

Match the nearest existing test. In-process tests use a local `CHECK` / `CHECK_EQ`. Socket scripts use `expect` / `expect_match`. A `compile_fail` program must not compile, and the diagnostic must match. `./dev check` is the release build plus `ctest`.
