#pragma once
// Static files, from a directory or from bytes embedded in the binary.
//
//   crocket::build()
//       .files({.prefix = "/static", .root = "web"})
//       .mount("/api", reflect_routes<^^api>())
//       .launch();
//
//   static constexpr unsigned char index_html[] = {
//   #embed "web/index.html"
//   };
//   .files({.assets = {asset("index.html", index_html)}})
//
// Routes are matched first, so a handler never waits on the filesystem. A miss
// is the ordinary 404. Only GET and HEAD read files. See the README.

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

/// Where `.files()` serves from. Set `root` or `assets`, not both.
/// An aggregate, so `.files({.prefix = "/static", .root = "web"})` works.
struct StaticFiles {
  std::string prefix = "/";
  std::string root = {};             // directory on disk, opened at ignite
  std::vector<Asset> assets = {};    // embedded bytes; empty when `root` is set
  std::string index = "index.html";  // empty: a directory is not a file
  bool dotfiles = false;             // a segment starting with '.' is a miss
  bool spa = false;                  // a miss whose last segment has no dot serves index
  std::string cache_control = {};    // empty means "no-cache"
  std::size_t max_bytes = std::size_t(32) << 20;
  /// Extra or replacement content types, extension to full header value
  /// (`{{"weba", "audio/webm"}}`). The extension has no dot. Text types in the
  /// built-in table already carry a charset.
  std::vector<std::pair<std::string, std::string>> types = {};
};

}  // namespace crocket
