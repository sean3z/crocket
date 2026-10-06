#include "router.hpp"

#include <algorithm>

namespace crocket::detail {

Captures Router::Matches::captures(const Route& r) const {
  Captures c;
  for (std::size_t i = 0; i < r.n_captures; ++i) c.push_back(parts_[r.capture_at[i]]);
  return c;
}

void Router::Matches::add(const Route* r) {
  if (big_.empty() && n_ < small_.size()) {
    small_[n_++] = r;
    return;
  }
  if (big_.empty()) big_.assign(small_.begin(), small_.begin() + n_);
  big_.push_back(r);
}

void Router::Matches::sort() {
  auto by_position = [](const Route* a, const Route* b) { return a->position < b->position; };
  if (big_.empty()) std::sort(small_.begin(), small_.begin() + n_, by_position);
  else std::ranges::sort(big_, by_position);
}

Router::Router(std::span<const RouteDef> routes) {
  std::size_t order = 0;
  struct Ordered {
    Route r;
    std::size_t order;
  };
  std::vector<Ordered> all;
  for (auto& def : routes) {
    Route r{&def, {}};
    (void)http::parse_template(def.path, r.segs);  // validated at ignite
    for (std::size_t i = 0; i < r.segs.size(); ++i)
      if (r.segs[i].capture && r.n_captures < Captures::capacity) r.capture_at[r.n_captures++] = std::uint8_t(i);
    all.push_back({std::move(r), order++});
  }
  // The candidate order: rank, then specificity, then registration.
  std::ranges::stable_sort(all, [](const Ordered& a, const Ordered& b) {
    if (a.r.def->rank != b.r.def->rank) return a.r.def->rank < b.r.def->rank;
    auto n = std::min(a.r.segs.size(), b.r.segs.size());
    for (std::size_t i = 0; i < n; ++i)
      if (a.r.segs[i].capture != b.r.segs[i].capture) return !a.r.segs[i].capture;
    return a.order < b.order;
  });
  routes_.reserve(all.size());  // stable addresses: the tries point into it
  for (auto& o : all) {
    o.r.position = routes_.size();
    routes_.push_back(std::move(o.r));
  }

  for (auto& r : routes_) {
    if (r.segs.size() > kMaxSegments) continue;  // could never match
    auto& trie = tries_[std::size_t(r.def->method)];
    if (trie.empty()) trie.emplace_back();
    std::uint32_t node = 0;
    for (auto& seg : r.segs) {
      if (seg.capture) {
        if (trie[node].capture == kNone) {
          trie[node].capture = std::uint32_t(trie.size());
          trie.emplace_back();
        }
        node = trie[node].capture;
        continue;
      }
      auto& lits = trie[node].literals;
      auto it = std::ranges::lower_bound(lits, seg.text, {}, &std::pair<std::string_view, std::uint32_t>::first);
      if (it == lits.end() || it->first != seg.text) {
        auto child = std::uint32_t(trie.size());
        lits.insert(it, {seg.text, child});
        trie.emplace_back();  // after the insert: `lits` refers into `trie`
        node = child;
      } else {
        node = it->second;
      }
    }
    trie[node].ends.push_back(&r);
  }
}

bool Router::split(std::string_view path, Matches& m) {
  if (path.empty() || path == "/") return true;
  if (path.front() == '/') path.remove_prefix(1);
  while (true) {
    if (m.n_parts_ == kMaxSegments) return false;
    auto slash = path.find('/');
    m.parts_[m.n_parts_++] = path.substr(0, slash);
    if (slash == std::string_view::npos) return true;
    path.remove_prefix(slash + 1);
  }
}

void Router::walk(const Trie& trie, std::uint32_t node, std::size_t i, Matches& m) const {
  const Node& n = trie[node];
  if (i == m.n_parts_) {
    for (auto* r : n.ends) m.add(r);
    return;
  }
  std::string_view part = m.parts_[i];
  auto it = std::ranges::lower_bound(n.literals, part, {}, &std::pair<std::string_view, std::uint32_t>::first);
  if (it != n.literals.end() && it->first == part) walk(trie, it->second, i + 1, m);
  if (n.capture != kNone && !part.empty()) walk(trie, n.capture, i + 1, m);
}

Router::Matches Router::match(http::Method method, std::string_view path) const {
  Matches m;
  if (!split(path, m)) return m;
  auto collect = [&](http::Method want) {
    auto& trie = tries_[std::size_t(want)];
    if (!trie.empty()) walk(trie, 0, 0, m);
  };
  collect(method);
  if (m.empty() && method == http::Method::Head) collect(http::Method::Get);
  m.sort();
  return m;
}

std::vector<http::Method> Router::allowed(std::string_view path) const {
  // Each method with a matching route, in the candidate order of its best match.
  std::vector<std::pair<std::size_t, http::Method>> found;
  for (std::size_t i = 0; i < tries_.size(); ++i) {
    if (tries_[i].empty()) continue;
    Matches m;
    if (!split(path, m)) return {};
    walk(tries_[i], 0, 0, m);
    m.sort();
    if (!m.empty()) found.emplace_back((*m.begin())->position, http::Method(i));
  }
  std::ranges::sort(found);
  std::vector<http::Method> out;
  for (auto& [pos, method] : found) out.push_back(method);
  if (std::ranges::find(out, http::Method::Get) != out.end() &&
      std::ranges::find(out, http::Method::Head) == out.end())
    out.push_back(http::Method::Head);
  return out;
}

std::string Router::shape(std::string_view templ) {
  std::vector<http::Segment> segs;
  (void)http::parse_template(templ, segs);
  std::string s;
  for (auto& seg : segs) {
    s += '/';
    s += seg.capture ? std::string_view("{}") : seg.text;
  }
  return s.empty() ? "/" : s;
}

}  // namespace crocket::detail
