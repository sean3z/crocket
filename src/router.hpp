#pragma once
// Route matching. Candidates for a request are ordered by explicit rank, then
// specificity (a literal segment beats a capture at the first difference),
// then mount order. Crocket tries them in order; a path capture that fails to
// parse forwards to the next candidate.
//
// Routes are compiled at ignite into one trie per method, keyed by path
// segment. A lookup walks it with the path split into a fixed array on the
// stack, and allocates nothing unless more than 32 routes match one path.

#include "crocket/app.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crocket::detail {

class Router {
 public:
  /// Paths with more segments than this match no route (404).
  static constexpr std::size_t kMaxSegments = 64;

  struct Route {
    const RouteDef* def;
    std::vector<http::Segment> segs;
    std::array<std::uint8_t, Captures::capacity> capture_at{};  // segment index of each capture
    std::size_t n_captures = 0;
    std::size_t position = 0;  // in the candidate order
  };

  /// The routes matching one path, best first.
  class Matches {
   public:
    [[nodiscard]] const Route* const* begin() const { return big_.empty() ? small_.data() : big_.data(); }
    [[nodiscard]] const Route* const* end() const { return begin() + size(); }
    [[nodiscard]] std::size_t size() const { return big_.empty() ? n_ : big_.size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    /// `r`'s capture values, from this path.
    [[nodiscard]] Captures captures(const Route& r) const;

   private:
    friend class Router;
    void add(const Route* r);
    void sort();
    std::array<std::string_view, kMaxSegments> parts_;
    std::size_t n_parts_ = 0;
    std::array<const Route*, 32> small_;
    std::size_t n_ = 0;
    std::vector<const Route*> big_;  // only past 32 matches
  };

  explicit Router(std::span<const RouteDef> routes);

  /// Candidates for method+path, best first. HEAD falls back to GET routes
  /// when no HEAD route matches.
  [[nodiscard]] Matches match(http::Method m, std::string_view path) const;

  /// Methods that have a route whose template matches `path` (for 405).
  [[nodiscard]] std::vector<http::Method> allowed(std::string_view path) const;

  /// "/users/{id}" -> "/users/{}" : two routes with the same method, shape and
  /// rank can never be told apart.
  static std::string shape(std::string_view templ);

 private:
  static constexpr std::uint32_t kNone = UINT32_MAX;
  struct Node {
    std::vector<std::pair<std::string_view, std::uint32_t>> literals;  // sorted by text
    std::uint32_t capture = kNone;
    std::vector<const Route*> ends;  // routes whose template ends here
  };
  using Trie = std::vector<Node>;  // node 0 is the root

  static bool split(std::string_view path, Matches& m);
  void walk(const Trie& trie, std::uint32_t node, std::size_t i, Matches& m) const;

  std::vector<Route> routes_;  // in candidate order
  std::array<Trie, std::size_t(http::Method::Unknown) + 1> tries_;
};

}  // namespace crocket::detail
