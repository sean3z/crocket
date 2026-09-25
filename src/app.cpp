#include "crocket/app.hpp"
#include "crocket/responder.hpp"
#include "engine.hpp"
#include "router.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

namespace crocket {

App::App(Config cfg) : core_(std::make_unique<detail::AppCore>()) { core_->config = std::move(cfg); }
App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;
App::~App() = default;

App& App::mount(std::string_view base, Routes routes) & {
  if (core_->ignited)
    core_->build_errors.push_back("mount(\"" + std::string(base) + "\") after ignite");
  core_->mounts.emplace_back(std::string(base), std::move(routes));
  return *this;
}

App& App::configure(Config cfg) & {
  core_->config = std::move(cfg);
  return *this;
}

namespace {

bool valid_request_id(std::string_view id) {
  if (id.empty() || id.size() > 128) return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

std::string describe(const RouteDef& r) {
  return std::string(http::method_name(r.method)) + " " + r.path + " (" + std::string(r.handler) + ")";
}

RouteDef builtin_route(std::string path, std::string_view handler, detail::Builtin b) {
  RouteDef d;
  d.method = http::Method::Get;
  d.path = std::move(path);
  d.handler = handler;
  d.builtin = b;
  return d;
}

}  // namespace

std::expected<void, IgniteError> App::ignite() {
  auto& c = *core_;
  if (c.ignited) return {};
  std::vector<std::string> errors = c.build_errors;

  // 1. Assemble the route table: mount prefixes + built-ins.
  Routes routes;
  for (auto& [base_in, list] : c.mounts) {
    std::string base = base_in;
    if (base.empty() || base.front() != '/') {
      errors.push_back("mount base \"" + base + "\" must start with '/'");
      continue;
    }
    if (base.find_first_of("{}") != std::string::npos) {
      errors.push_back("mount base \"" + base + "\" must not contain captures");
      continue;
    }
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    for (auto r : list) {
      if (base != "/") r.path = r.path == "/" ? base : base + r.path;
      std::vector<http::Segment> segs;
      if (auto err = http::parse_template(r.path, segs); !err.empty())
        errors.push_back(describe(r) + ": " + err);
      routes.push_back(std::move(r));
    }
  }
  routes.push_back(builtin_route("/healthz", "crocket::healthz", detail::Builtin::Healthz));
  routes.push_back(builtin_route("/readyz", "crocket::readyz", detail::Builtin::Readyz));
  if (c.config.debug_routes)
    routes.push_back(builtin_route("/__routes", "crocket::routes", detail::Builtin::Routes));

  // 2. Every State<T> a route needs must be managed.
  std::set<std::string> seen;
  for (auto& r : routes)
    for (auto& need : r.needs)
      if (!c.state.find(need.key)) {
        auto msg = describe(r) + " requires State<" + std::string(need.name) + ">, but " + std::string(need.name) +
                   " is not managed; add .manage(" + std::string(need.name) + "{...})";
        if (seen.insert(msg).second) errors.push_back(std::move(msg));
      }

  // 3. Same method + same shape + same rank can never be disambiguated.
  std::map<std::tuple<http::Method, std::string, int>, const RouteDef*> shapes;
  for (auto& r : routes) {
    auto key = std::make_tuple(r.method, detail::Router::shape(r.path), r.rank);
    auto [it, inserted] = shapes.emplace(key, &r);
    if (!inserted)
      errors.push_back("route conflict: " + describe(*it->second) + " and " + describe(r) +
                       " match the same requests at rank " + std::to_string(r.rank) +
                       "; give one of them a different rank");
  }

  c.routes = std::move(routes);

  // 4. Fairings get the last word.
  Ignite ig(c, errors);
  for (auto& f : c.fairings) {
    try {
      f->on_ignite(ig);
    } catch (const std::exception& e) {
      errors.push_back("fairing " + std::string(f->name()) + " threw during ignite: " + e.what());
    }
  }

  if (!errors.empty()) {
    c.routes.clear();
    return std::unexpected(IgniteError{std::move(errors)});
  }
  c.router = std::make_shared<detail::Router>(c.routes);
  c.ignited = true;
  return {};
}

void App::prepare(Request& req) const {
  if (req.request_id.empty()) {
    auto hdr = req.header("x-request-id");
    req.request_id = (hdr && valid_request_id(*hdr)) ? std::string(*hdr) : detail::generate_request_id();
  }
  if (req.deadline.at == Clock::time_point::max()) req.deadline.at = req.received + core_->config.request_timeout;
  req.state_registry = &core_->state;
}

Response App::handle(Request req) {
  prepare(req);
  core_->stats.in_flight.fetch_add(1, std::memory_order_relaxed);
  struct Dec {
    detail::Stats& s;
    ~Dec() { s.in_flight.fetch_sub(1, std::memory_order_relaxed); }
  } dec{core_->stats};

  Response res;
  bool finished = false;
  for (auto& f : core_->fairings) {
    try {
      if (auto early = f->on_request(req)) {
        res = std::move(*early);
        finished = true;
        break;
      }
    } catch (const std::exception& e) {
      write_error(ApiError::internal("fairing " + std::string(f->name()) + " on_request threw: " + e.what()), req, res);
      finished = true;
      break;
    }
  }
  if (!finished) res = run_routes(req);
  finish(req, res);
  return res;
}

Response App::reject(Request& req, const ApiError& err) {
  prepare(req);
  Response res;
  write_error(err, req, res);
  finish(req, res);
  return res;
}

void App::finish(const Request& req, Response& res) {
  res.headers.set("x-request-id", req.request_id);
  auto& fs = core_->fairings;
  for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
    try {
      (*it)->on_response(req, res);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "crocket: fairing %.*s on_response threw: %s\n", int((*it)->name().size()),
                   (*it)->name().data(), e.what());
    }
  }
}

Response App::run_routes(Request& req) {
  Response res;
  if (!core_->ignited) {
    write_error(ApiError::internal("request handled before ignite"), req, res);
    return res;
  }
  auto candidates = core_->router->match(req.method, req.path);
  bool forwarded = false;
  std::string forward_detail;
  for (auto& cand : candidates) {
    req.route_template = cand.def->path;
    req.handler = cand.def->handler;
    req.captures = std::move(cand.captures);
    if (cand.def->builtin != detail::Builtin::None) return builtin(cand.def->builtin, req);

    res = Response{};
    try {
      auto out = cand.def->invoke(req, res);
      if (out.kind == detail::Outcome::Done) return res;
      if (out.kind == detail::Outcome::Forward) {
        forwarded = true;
        forward_detail = std::string(req.handler) + ": " + out.detail;
        continue;
      }
      res = Response{};
      write_error(out.error, req, res);
      res.failure_kind = out.failure_kind;
      return res;
    } catch (const ApiError& e) {
      res = Response{};
      write_error(e, req, res);
      return res;
    } catch (const std::exception& e) {
      res = Response{};
      write_error(ApiError::internal(std::string("uncaught exception in ") + std::string(req.handler) + ": " + e.what()),
                  req, res);
      return res;
    } catch (...) {
      res = Response{};
      write_error(ApiError::internal("uncaught non-std exception in " + std::string(req.handler)), req, res);
      return res;
    }
  }

  res = Response{};
  if (forwarded) {
    write_error(ApiError{404, "path.invalid", "not found", forward_detail}, req, res);
    res.failure_kind = "path";
    return res;
  }
  req.route_template = {};
  req.handler = {};
  auto allowed = core_->router->allowed(req.path);
  if (!allowed.empty()) {
    std::string allow;
    for (auto m : allowed) {
      if (!allow.empty()) allow += ", ";
      allow += http::method_name(m);
    }
    write_error(ApiError{405, "method.not_allowed", "method not allowed", {}}, req, res);
    res.headers.set("allow", allow);
    return res;
  }
  write_error(ApiError::not_found(), req, res);
  return res;
}

Response App::builtin(detail::Builtin b, Request& req) {
  Response res;
  res.set_content_type("application/json");
  res.headers.set("cache-control", "no-store");
  switch (b) {
    case detail::Builtin::Healthz:
      res.body = R"({"status":"ok"})";
      break;
    case detail::Builtin::Readyz: {
      if (core_->stats.draining.load()) {
        res.status = 503;
        res.error_code = "server.draining";
        res.body = R"({"status":"draining"})";
        break;
      }
      bool ok = true;
      std::string checks;
      for (auto& rc : core_->ready_checks) {
        bool pass = false;
        try {
          pass = rc.check(core_->state.find(rc.key));
        } catch (...) {
        }
        ok = ok && pass;
        if (!checks.empty()) checks += ',';
        json::write_string(checks, rc.name);
        checks += pass ? R"(:"ok")" : R"(:"fail")";
      }
      res.status = ok ? 200 : 503;
      if (!ok) res.error_code = "not_ready";
      res.body = std::string(R"({"status":)") + (ok ? R"("ready")" : R"("not_ready")") + R"(,"checks":{)" + checks + "}}";
      break;
    }
    case detail::Builtin::Routes: {
      std::string& o = res.body;
      o = "[";
      bool first = true;
      for (auto& r : core_->routes) {
        if (!first) o += ',';
        first = false;
        o += R"({"method":)";
        json::write_string(o, http::method_name(r.method));
        o += R"(,"path":)";
        json::write_string(o, r.path);
        o += R"(,"handler":)";
        json::write_string(o, r.handler);
        o += R"(,"rank":)" + std::to_string(r.rank) + R"(,"params":[)";
        for (std::size_t i = 0; i < r.params.size(); ++i) {
          if (i) o += ',';
          o += R"({"name":)";
          json::write_string(o, r.params[i].name);
          o += R"(,"source":)";
          json::write_string(o, r.params[i].source);
          o += R"(,"type":)";
          json::write_string(o, r.params[i].type);
          o += '}';
        }
        o += "]}";
      }
      o += ']';
      break;
    }
    case detail::Builtin::None:
      break;
  }
  return res;
}

int App::listen(ListenOptions opts) {
  auto ig = ignite();
  std::vector<std::string> problems = ig ? std::vector<std::string>{} : ig.error().problems;
  bool tls = !opts.tls_cert.empty() || !opts.tls_key.empty();
  if (tls) {
    if (opts.tls_cert.empty() || opts.tls_key.empty())
      problems.push_back("TLS needs both tls_cert and tls_key");
    for (auto* f : {&opts.tls_cert, &opts.tls_key})
      if (!f->empty() && !std::ifstream(*f).good()) problems.push_back("TLS file not readable: " + *f);
  }
  if (!problems.empty()) {
    std::fprintf(stderr, "%s\n", IgniteError{problems}.message().c_str());
    return 1;
  }
  return detail::run_engine(*this, opts);
}

namespace detail {
void shutdown_app(App& app) {
  auto& fs = app.core().fairings;
  for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
    try {
      (*it)->on_shutdown();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "crocket: fairing on_shutdown threw: %s\n", e.what());
    }
  }
  app.core().state.clear();
}
}  // namespace detail

// ---- LocalClient -------------------------------------------------------------

LocalClient::LocalClient(App& app) : app_(app) {
  if (auto r = app_.ignite(); !r) throw std::runtime_error(r.error().message());
}

LocalClient::Call::Call(App& app, http::Method m, std::string_view target) : app_(app) {
  req_.method = m;
  req_.method_text = http::method_name(m);
  auto q = target.find('?');
  req_.path = detail::percent_decode(target.substr(0, q), false);
  if (q != std::string_view::npos) req_.query = detail::parse_query(target.substr(q + 1));
  req_.received = Clock::now();
}

}  // namespace crocket
