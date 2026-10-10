// The OpenAPI document: every HTTP route's operation, described by the
// function reflect_routes() gave it (include/crocket/detail/openapi.hpp), plus
// the shared schemas, assembled once at ignite and served by the OpenApi fairing.

#include "crocket/detail/openapi.hpp"

#include "crocket/fairings.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>

namespace crocket {
namespace detail::openapi {

std::string Components::name(const void* key, std::string_view short_name, std::string_view qualified, bool& fresh) {
  if (auto it = by_key_.find(key); it != by_key_.end()) {
    fresh = false;
    return it->second;
  }
  // The short name if it is free, else the qualified one, else numbered.
  std::string n(short_name);
  if (taken_.contains(n)) n = std::string(qualified);
  for (int i = 2; taken_.contains(n); ++i) n = std::string(qualified) + "_" + std::to_string(i);
  taken_.insert(n);
  by_key_.emplace(key, n);
  fresh = true;
  return n;
}

void Components::define(const std::string& name, Value schema) { defs_.emplace_back(name, std::move(schema)); }

Object Components::schemas() const { return Object(defs_.begin(), defs_.end()); }

Value schema_ref(std::string_view component) { return Object{{"$ref", "#/components/schemas/" + std::string(component)}}; }

Value nullable(Value s) {
  // {"type": "string"} -> {"type": ["string", "null"]}; anything else -> anyOf.
  if (s.is_object()) {
    auto* type = s.find("type");
    if (type && type->is_string() && !s.find("enum")) {
      Object o = s.as_object();
      for (auto& [k, v] : o)
        if (k == "type") v = Array{v, std::string("null")};
      return o;
    }
  }
  return Object{{"anyOf", Array{std::move(s), Object{{"type", std::string("null")}}}}};
}

Value problem_schema() {
  // What write_error sends (RFC 9457).
  auto str = [] { return Value(Object{{"type", std::string("string")}}); };
  return Object{
      {"type", std::string("object")},
      {"description", std::string("An error, as RFC 9457 problem details")},
      {"properties",
       Object{{"type", str()},
              {"title", str()},
              {"status", Object{{"type", std::string("integer")}}},
              {"detail", str()},
              {"code", Object{{"type", std::string("string")}, {"description", std::string("Stable, for programs: json.invalid")}}},
              {"request_id", str()},
              {"errors", Object{{"type", std::string("array")},
                                {"items", Object{{"type", std::string("object")},
                                                 {"properties", Object{{"pointer", str()}, {"detail", str()}}}}}}}}},
      {"required", Array{std::string("title"), std::string("status"), std::string("detail"), std::string("code")}}};
}

void Operation::parameter(std::string_view in, std::string_view name, bool required, Value schema) {
  for (auto& p : parameters)
    if (p.find("name")->as_string() == name && p.find("in")->as_string() == in) return;
  parameters.push_back(Object{{"name", std::string(name)},
                              {"in", std::string(in)},
                              {"required", required},
                              {"schema", std::move(schema)}});
}

namespace {
std::string success_description(std::string_view code) {
  if (code == "2XX") return "Success";
  if (code.size() == 3 && std::isdigit(static_cast<unsigned char>(code[0])))
    return std::string(reason_phrase(std::stoi(std::string(code))));
  return "Success";
}
}  // namespace

void Operation::respond(std::string code, std::string_view media, std::optional<Value> schema) {
  for (auto& [c, r] : responses)
    if (c == code) return;
  Object r{{"description", success_description(code)}};
  if (!media.empty()) {
    Object m;
    if (schema) m.emplace_back("schema", std::move(*schema));
    r.emplace_back("content", Object{{std::string(media), std::move(m)}});
  }
  responses.emplace_back(std::move(code), std::move(r));
}

void Operation::error(int status, std::string_view when) {
  auto code = std::to_string(status);
  for (auto& [c, r] : responses)
    if (c == code) return;
  responses.emplace_back(
      code, Object{{"description", std::string(reason_phrase(status)) + ": " + std::string(when)},
                   {"content", Object{{"application/problem+json", Object{{"schema", schema_ref("Problem")}}}}}});
}

}  // namespace detail::openapi

namespace {

namespace oa = detail::openapi;
using json::Array;
using json::Object;
using json::Value;

constexpr char kProblemKey = 0;

/// "api::create_user" -> "Create user"; "api::getUser" -> "Get user".
std::string summary_of(std::string_view handler) {
  auto at = handler.rfind("::");
  std::string_view fn = at == std::string_view::npos ? handler : handler.substr(at + 2);
  std::string out;
  for (std::size_t i = 0; i < fn.size(); ++i) {
    char c = fn[i];
    if (c == '_') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else if (std::isupper(static_cast<unsigned char>(c)) && i > 0) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += char(std::tolower(static_cast<unsigned char>(c)));
    } else {
      out += c;
    }
  }
  if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

/// "api::orders::create" -> "api.orders"; "" for a handler at global scope.
std::string tag_of(std::string_view handler) {
  auto at = handler.rfind("::");
  if (at == std::string_view::npos) return {};
  std::string out;
  for (std::size_t i = 0; i < at; ++i) {
    if (handler[i] == ':' && i + 1 < at && handler[i + 1] == ':') {
      out += '.';
      ++i;
    } else {
      out += handler[i];
    }
  }
  return out;
}

std::string operation_id(std::string_view handler, std::set<std::string>& used) {
  std::string id;
  for (std::size_t i = 0; i < handler.size(); ++i) {
    if (handler[i] == ':' && i + 1 < handler.size() && handler[i + 1] == ':') {
      id += '.';
      ++i;
    } else {
      id += handler[i];
    }
  }
  std::string unique = id;
  for (int n = 2; !used.insert(unique).second; ++n) unique = id + "_" + std::to_string(n);  // mounted twice
  return unique;
}

void describe_builtin(const RouteDef& r, oa::Operation& op) {
  auto status_body = [](std::initializer_list<const char*> values) {
    Array e;
    for (auto* v : values) e.push_back(std::string(v));
    return Value(Object{{"type", std::string("object")},
                        {"properties", Object{{"status", Object{{"type", std::string("string")}, {"enum", e}}}}}});
  };
  if (r.builtin == detail::Builtin::Healthz) {
    op.respond("200", "application/json", status_body({"ok"}));
  } else if (r.builtin == detail::Builtin::Readyz) {
    op.respond("200", "application/json", status_body({"ready"}));
    op.responses.emplace_back("503", Object{{"description", std::string("Not ready, or draining")},
                                            {"content", Object{{"application/json", Object{{"schema", status_body({"not_ready", "draining"})}}}}}});
  }
}

/// Success codes first, in order; then errors by code; "default" last.
void order_responses(std::vector<std::pair<std::string, Value>>& rs) {
  std::ranges::stable_sort(rs, [](auto& a, auto& b) {
    auto rank = [](const std::string& c) { return c.empty() || c[0] > '3' ? 1 : 0; };
    if (rank(a.first) != rank(b.first)) return rank(a.first) < rank(b.first);
    return rank(a.first) == 1 && a.first < b.first;
  });
}

std::string build_document(std::span<const RouteDef> routes, const OpenApi::Options& o) {
  oa::Components components;
  bool fresh = false;
  components.define(components.name(&kProblemKey, "Problem", "crocket.Problem", fresh), oa::problem_schema());

  std::vector<std::pair<std::string, Object>> paths;  // template -> methods, in route order
  std::set<std::pair<std::string, std::string>> seen;
  std::set<std::string> ids;
  bool bearer = false;
  static constexpr std::string_view kMethods[] = {"get", "put", "post", "delete", "options", "head", "patch", "trace"};

  for (const auto& r : routes) {
    if (r.mode == Mode::Grpc || r.builtin == detail::Builtin::Routes) continue;
    std::string method(http::method_name(r.method));
    for (auto& c : method) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (std::ranges::find(kMethods, method) == std::end(kMethods)) continue;
    // Several ranks of one template and method: the first answers.
    if (!seen.emplace(r.path, method).second) continue;

    oa::Operation op{components};
    if (r.openapi) r.openapi(op);
    else describe_builtin(r, op);
    if (op.responses.empty()) op.respond("200");
    op.responses.emplace_back(
        "default", Object{{"description", std::string("An error")},
                          {"content", Object{{"application/problem+json", Object{{"schema", oa::schema_ref("Problem")}}}}}});
    order_responses(op.responses);

    Object operation;
    if (auto tag = tag_of(r.handler); !tag.empty()) operation.emplace_back("tags", Array{tag});
    operation.emplace_back("summary", summary_of(r.handler));
    operation.emplace_back("operationId", operation_id(r.handler, ids));
    if (!op.parameters.empty()) operation.emplace_back("parameters", std::move(op.parameters));
    if (op.body) operation.emplace_back("requestBody", std::move(*op.body));
    operation.emplace_back("responses", Object(op.responses.begin(), op.responses.end()));
    if (op.bearer) {
      operation.emplace_back("security", Array{Object{{"bearer", Array{}}}});
      bearer = true;
    }

    auto it = std::ranges::find(paths, r.path, &std::pair<std::string, Object>::first);
    if (it == paths.end()) {
      paths.emplace_back(r.path, Object{});
      it = paths.end() - 1;
    }
    it->second.emplace_back(method, std::move(operation));
  }

  Object info{{"title", o.title}, {"version", o.version}};
  if (!o.description.empty()) info.emplace_back("description", o.description);
  Object path_items;
  for (auto& [p, methods] : paths) path_items.emplace_back(p, std::move(methods));
  Object comps{{"schemas", components.schemas()}};
  if (bearer)
    comps.emplace_back("securitySchemes",
                       Object{{"bearer", Object{{"type", std::string("http")}, {"scheme", std::string("bearer")}}}});
  Value doc = Object{{"openapi", std::string("3.1.0")},
                     {"info", std::move(info)},
                     {"paths", std::move(path_items)},
                     {"components", std::move(comps)}};
  std::string out;
  json::dump(doc, out);
  return out;
}

std::string docs_page(const OpenApi::Options& o) {
  auto escape = [](std::string_view s) {
    std::string out;
    for (char c : s) {
      if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;";
      else if (c == '&') out += "&amp;";
      else if (c == '"') out += "&quot;";
      else out += c;
    }
    return out;
  };
  return "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
         escape(o.title) +
         "</title>\n<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css\">\n"
         "</head>\n<body>\n<div id=\"ui\" data-spec=\"" +
         escape(o.path) +
         "\"></div>\n<script src=\"https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js\"></script>\n"
         "<script src=\"" +
         escape(o.docs) + "/init.js\"></script>\n</body>\n</html>\n";
}

constexpr std::string_view kDocsScript =
    "const ui = document.getElementById('ui');\n"
    "SwaggerUIBundle({ url: ui.dataset.spec, domNode: ui, deepLinking: true, validatorUrl: null });\n";

// Shield's policy allows nothing; the page needs Swagger UI from the CDN, its
// inline styles and data: fonts, and the document from here. No inline
// script: init.js is served next to the page.
constexpr std::string_view kDocsPolicy =
    "default-src 'none'; script-src 'self' https://cdn.jsdelivr.net; "
    "style-src 'unsafe-inline' https://cdn.jsdelivr.net; img-src 'self' data: https://cdn.jsdelivr.net; "
    "font-src data: https://cdn.jsdelivr.net; connect-src 'self'; frame-ancestors 'none'";

}  // namespace

OpenApi::OpenApi(Options o) : o_(std::move(o)) {}

void OpenApi::on_ignite(Ignite& ig) {
  if (o_.path.empty() || o_.path.front() != '/') ig.fail("OpenApi: path \"" + o_.path + "\" must start with '/'");
  if (!o_.docs.empty() && o_.docs.front() != '/') ig.fail("OpenApi: docs \"" + o_.docs + "\" must start with '/'");
  doc_ = std::make_shared<const std::string>(build_document(ig.routes(), o_));
}

std::optional<Response> OpenApi::on_request(Request& rq) {
  if (rq.method != http::Method::Get && rq.method != http::Method::Head) return std::nullopt;
  Response rs;
  if (rq.path == o_.path) {
    rs.set_content_type("application/json");
    rs.body = *doc_;
  } else if (!o_.docs.empty() && rq.path == o_.docs) {
    rs.set_content_type("text/html; charset=utf-8");
    rs.headers.set("content-security-policy", kDocsPolicy);
    rs.body = docs_page(o_);
  } else if (!o_.docs.empty() && rq.path == o_.docs + "/init.js") {
    rs.set_content_type("text/javascript; charset=utf-8");
    rs.body = kDocsScript;
  } else {
    return std::nullopt;
  }
  rq.route_template = rq.path == o_.path ? o_.path : o_.docs;
  return rs;
}

std::string openapi_document(Crocket& app, const OpenApi::Options& o) {
  if (auto r = app.ignite(); !r) throw std::runtime_error(r.error().message());
  return build_document(app.core().routes, o);
}

}  // namespace crocket
