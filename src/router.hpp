#pragma once
// Route matching. Candidates for a request are ordered by explicit rank, then
// specificity (a literal segment beats a capture at the first difference),
// then mount order. The App tries them in order; a path capture that fails to
// parse forwards to the next candidate.

#include "crocket/app.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crocket::detail {

class Router {
 public:
  struct Candidate {
    const RouteDef* def;
    std::vector<std::string_view> captures;
  };

  explicit Router(std::span<const RouteDef> routes);

  /// Candidates for method+path, best first. HEAD falls back to GET routes
  /// when no HEAD route matches.
  [[nodiscard]] std::vector<Candidate> match(http::Method m, std::string_view path) const;

  /// Methods that have a route whose template matches `path` (for 405).
  [[nodiscard]] std::vector<http::Method> allowed(std::string_view path) const;

  /// "/users/{id}" -> "/users/{}" : two routes with the same method, shape and
  /// rank can never be told apart.
  static std::string shape(std::string_view templ);

 private:
  struct Compiled {
    const RouteDef* def;
    std::vector<http::Segment> segs;
    std::size_t order;
  };
  static bool matches(const Compiled& c, const std::vector<std::string_view>& parts,
                      std::vector<std::string_view>* captures);
  std::vector<Compiled> routes_;
};

std::vector<std::string_view> split_path(std::string_view path);

}  // namespace crocket::detail
