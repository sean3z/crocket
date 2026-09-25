#include "router.hpp"

#include <algorithm>

namespace crocket::detail {

std::vector<std::string_view> split_path(std::string_view path) {
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

Router::Router(std::span<const RouteDef> routes) {
  std::size_t order = 0;
  for (auto& r : routes) {
    Compiled c{&r, {}, order++};
    (void)http::parse_template(r.path, c.segs);  // validated at ignite
    routes_.push_back(std::move(c));
  }
  // Stable global order: rank, then specificity, then registration.
  std::ranges::stable_sort(routes_, [](const Compiled& a, const Compiled& b) {
    if (a.def->rank != b.def->rank) return a.def->rank < b.def->rank;
    auto n = std::min(a.segs.size(), b.segs.size());
    for (std::size_t i = 0; i < n; ++i)
      if (a.segs[i].capture != b.segs[i].capture) return !a.segs[i].capture;
    return a.order < b.order;
  });
}

bool Router::matches(const Compiled& c, const std::vector<std::string_view>& parts,
                     std::vector<std::string_view>* captures) {
  if (c.segs.size() != parts.size()) return false;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (c.segs[i].capture) {
      if (parts[i].empty()) return false;
    } else if (c.segs[i].text != parts[i]) {
      return false;
    }
  }
  if (captures) {
    captures->clear();
    for (std::size_t i = 0; i < parts.size(); ++i)
      if (c.segs[i].capture) captures->push_back(parts[i]);
  }
  return true;
}

std::vector<Router::Candidate> Router::match(http::Method m, std::string_view path) const {
  auto parts = split_path(path);
  std::vector<Candidate> out;
  auto collect = [&](http::Method want) {
    for (auto& c : routes_) {
      if (c.def->method != want) continue;
      std::vector<std::string_view> caps;
      if (matches(c, parts, &caps)) out.push_back({c.def, std::move(caps)});
    }
  };
  collect(m);
  if (out.empty() && m == http::Method::Head) collect(http::Method::Get);
  return out;
}

std::vector<http::Method> Router::allowed(std::string_view path) const {
  auto parts = split_path(path);
  std::vector<http::Method> out;
  for (auto& c : routes_)
    if (matches(c, parts, nullptr) && std::ranges::find(out, c.def->method) == out.end())
      out.push_back(c.def->method);
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
