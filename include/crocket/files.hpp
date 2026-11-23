#pragma once
// Static files, from a directory or from bytes embedded in the binary.
//
//   crocket::build()
//       .mount("/", files{
//           .root = "web",
//           .spa = true,
//           .precompressed = true,
//           .cache = {{"*.html", "no-cache"},
//                     {"assets/*", "public, max-age=31536000, immutable"}},
//           .headers = {{"*.html", {{"content-security-policy", "default-src 'self'"}}}}})
//       .mount("/api", reflect_routes<^^api>())
//       .launch();
//
//   static constexpr unsigned char index_html[] = {
//   #embed "web/index.html"
//   };
//   .mount("/", files{.assets = {asset("index.html", index_html)}})
//
// Routes match before files, whichever you mount first, so a handler never
// waits on the filesystem. A miss is the ordinary 404 unless `.fallthrough`
// is set. Only GET and HEAD read files. See the README.

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crocket {

/// One compile-time file. `path` is relative to the mount ("js/app.js").
/// `bytes` must outlive the process; a constexpr array does.
struct Asset {
  std::string_view path;
  std::span<const std::byte> bytes;
};

/// `data` is the file's bytes, as `#embed` produces. A string literal's trailing
/// NUL is part of the array, so pass an embed, not a literal.
template <class T, std::size_t N>
  requires(sizeof(T) == 1)
constexpr Asset asset(std::string_view path, const T (&data)[N]) noexcept {
  return {path, std::as_bytes(std::span<const T>(data, N))};
}

constexpr Asset asset(std::string_view path, std::span<const std::byte> bytes) noexcept {
  return {path, bytes};
}

/// One Cache-Control rule. `glob` is matched against the path relative to the
/// mount (`index.html`, `assets/app.js`). `*` matches any run of characters
/// except `/`, so `*.html` matches `index.html` and not `dir/index.html`.
/// The first matching rule wins.
struct CacheRule {
  std::string glob;
  std::string control;
};

/// Extra response headers for files whose relative path matches `glob`.
/// The first matching rule wins. Shield leaves a header the response already
/// has, so these replace Shield's value for the same name.
struct HeaderRule {
  std::string glob;
  std::vector<std::pair<std::string, std::string>> fields;
};

/// What a `.mount(prefix, files{...})` serves. Set `root` or `assets`, not both.
/// The URL prefix is the mount's first argument, not a field here.
/// An aggregate, so `.mount("/static", files{.root = "web"})` works.
struct files {
  std::string root = {};           // directory on disk, opened at ignite
  std::vector<Asset> assets = {};  // embedded bytes; empty when `root` is set
  std::string index = "index.html";  // empty: a directory is not a file
  bool dotfiles = false;             // a segment starting with '.' is a miss
  bool spa = false;                  // a miss whose last segment has no dot serves index
  std::string cache_control = {};    // used when no `.cache` rule matches; empty means "no-cache"
  bool precompressed = false;        // serve a sibling .br, then .gz, when Accept-Encoding allows
  bool fallthrough = false;          // a missing file is tried on the next files mount
  std::vector<CacheRule> cache = {};
  std::vector<HeaderRule> headers = {};
  std::size_t max_bytes = std::size_t(32) << 20;
  /// Small-file cache, outside the dev profile. Dev reads the disk on every
  /// request, including HEAD, and still sends no body. `memory` holds files up
  /// to `memory_file` bytes, and `memory_bytes` in all. A hit fresher than
  /// `memory_revalidate` is served with no syscall;
  /// after that, one fstat checks size and mtime before the cached bytes are
  /// reused. A change or a delete shows up on that check.
  bool memory = true;
  std::size_t memory_file = 256 * 1024;
  std::size_t memory_bytes = std::size_t(8) << 20;
  std::chrono::milliseconds memory_revalidate{1000};
  /// Extra or replacement content types, extension to full header value
  /// (`{{"weba", "audio/webm"}}`). The extension has no dot. Text types in the
  /// built-in table already carry a charset.
  std::vector<std::pair<std::string, std::string>> types = {};
};

}  // namespace crocket
