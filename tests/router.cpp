// The trie router against the linear scan it replaced: random route tables and
// paths must give the same candidates, in the same order, with the same
// captures, and the same methods for 405.

#include <crocket/crocket.hpp>

#include "router.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace crocket;

namespace {

/// The previous router: every route, in candidate order, checked one by one.
struct Reference {
  struct Compiled {
    const RouteDef* def;
    std::vector<http::Segment> segs;
    std::size_t order;
  };
  std::vector<Compiled> routes;

  explicit Reference(const Routes& defs) {
    std::size_t order = 0;
    for (auto& r : defs) {
      Compiled c{&r, {}, order++};
      (void)http::parse_template(r.path, c.segs);
      routes.push_back(std::move(c));
    }
    std::ranges::stable_sort(routes, [](const Compiled& a, const Compiled& b) {
      if (a.def->rank != b.def->rank) return a.def->rank < b.def->rank;
      auto n = std::min(a.segs.size(), b.segs.size());
      for (std::size_t i = 0; i < n; ++i)
        if (a.segs[i].capture != b.segs[i].capture) return !a.segs[i].capture;
      return a.order < b.order;
    });
  }
  static std::vector<std::string_view> split(std::string_view path) {
    std::vector<std::string_view> parts;
    if (path.empty() || path == "/") return parts;
    if (path.front() == '/') path.remove_prefix(1);
    while (true) {
      auto slash = path.find('/');
      parts.push_back(path.substr(0, slash));
      if (slash == std::string_view::npos) break;
      path.remove_prefix(slash + 1);
    }
    return parts;
  }
  static bool matches(const Compiled& c, const std::vector<std::string_view>& parts, std::vector<std::string_view>* caps) {
    if (c.segs.size() != parts.size()) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (c.segs[i].capture) {
        if (parts[i].empty()) return false;
      } else if (c.segs[i].text != parts[i]) {
        return false;
      }
    }
    if (caps)
      for (std::size_t i = 0; i < parts.size(); ++i)
        if (c.segs[i].capture) caps->push_back(parts[i]);
    return true;
  }
  std::vector<std::pair<const RouteDef*, std::vector<std::string_view>>> match(http::Method m, std::string_view path) const {
    auto parts = split(path);
    std::vector<std::pair<const RouteDef*, std::vector<std::string_view>>> out;
    auto collect = [&](http::Method want) {
      for (auto& c : routes) {
        if (c.def->method != want) continue;
        std::vector<std::string_view> caps;
        if (matches(c, parts, &caps)) out.emplace_back(c.def, std::move(caps));
      }
    };
    collect(m);
    if (out.empty() && m == http::Method::Head) collect(http::Method::Get);
    return out;
  }
  std::vector<http::Method> allowed(std::string_view path) const {
    auto parts = split(path);
    std::vector<http::Method> out;
    for (auto& c : routes)
      if (matches(c, parts, nullptr) && std::ranges::find(out, c.def->method) == out.end()) out.push_back(c.def->method);
    if (std::ranges::find(out, http::Method::Get) != out.end() && std::ranges::find(out, http::Method::Head) == out.end())
      out.push_back(http::Method::Head);
    return out;
  }
};

}  // namespace

int main() {
  std::mt19937 rng(20261006);
  auto pick = [&](auto& items) -> decltype(auto) { return items[std::uniform_int_distribution<std::size_t>(0, items.size() - 1)(rng)]; };
  const std::vector<std::string> route_segs = {"a", "b", "users", "{x}", "{y}", "{id}"};
  const std::vector<std::string> path_segs = {"a", "b", "users", "7", "", "zz"};
  const std::vector<http::Method> methods = {http::Method::Get, http::Method::Head, http::Method::Post, http::Method::Put};

  int failures = 0, tables = 0, lookups = 0;
  for (int t = 0; t < 2000; ++t) {
    // A random table: duplicate templates are fine here (ignite would reject them).
    Routes defs;
    int n = std::uniform_int_distribution<int>(1, 25)(rng);
    for (int i = 0; i < n; ++i) {
      std::string path;
      int depth = std::uniform_int_distribution<int>(0, 4)(rng);
      std::vector<std::string> used;
      for (int d = 0; d < depth; ++d) {
        std::string seg = pick(route_segs);
        if (seg.front() == '{' && std::ranges::find(used, seg) != used.end()) seg = "lit" + std::to_string(d);
        used.push_back(seg);
        path += "/" + seg;
      }
      RouteDef def;
      def.method = pick(methods);
      def.path = path.empty() ? "/" : path;
      def.rank = std::uniform_int_distribution<int>(0, 2)(rng);
      defs.push_back(std::move(def));
    }
    ++tables;
    Reference ref(defs);
    detail::Router trie(defs);

    for (int q = 0; q < 50; ++q) {
      std::string path;
      int depth = std::uniform_int_distribution<int>(0, 5)(rng);
      for (int d = 0; d < depth; ++d) path += "/" + pick(path_segs);
      if (path.empty() || std::uniform_int_distribution<int>(0, 9)(rng) == 0) path += "/";
      for (auto m : methods) {
        ++lookups;
        auto want = ref.match(m, path);
        auto got = trie.match(m, path);
        bool same = want.size() == got.size();
        std::size_t i = 0;
        for (const auto* r : got) {
          if (!same) break;
          auto caps = got.captures(*r);
          same = r->def == want[i].first && std::ranges::equal(caps, want[i].second);
          ++i;
        }
        if (!same && failures++ < 10) {
          std::fprintf(stderr, "  FAIL table %d: %s %s: %zu candidates, expected %zu\n", t,
                       std::string(http::method_name(m)).c_str(), path.c_str(), got.size(), want.size());
          for (auto& d : defs)
            std::fprintf(stderr, "      %s %s rank %d\n", std::string(http::method_name(d.method)).c_str(), d.path.c_str(), d.rank);
        }
      }
      if (ref.allowed(path) != trie.allowed(path) && failures++ < 10)
        std::fprintf(stderr, "  FAIL table %d: allowed(%s) differs\n", t, path.c_str());
    }
  }

  // Too deep to route, and many matches for one path (past the inline buffer).
  {
    Routes defs;
    for (int i = 0; i < 40; ++i) defs.push_back({.method = http::Method::Get, .path = "/{a}/x", .rank = i});
    detail::Router trie(defs);
    auto got = trie.match(http::Method::Get, "/7/x");
    if (got.size() != 40 || (*got.begin())->def->rank != 0 || (*(got.end() - 1))->def->rank != 39) {
      ++failures;
      std::fprintf(stderr, "  FAIL 40 matches: got %zu\n", got.size());
    }
    std::string deep;
    for (int i = 0; i < 70; ++i) deep += "/x";
    if (!trie.match(http::Method::Get, deep).empty()) {
      ++failures;
      std::fprintf(stderr, "  FAIL a 70-segment path matched\n");
    }
  }

  std::fprintf(stderr, "%d route tables, %d lookups, %d failures\n", tables, lookups, failures);
  return failures == 0 ? 0 : 1;
}
