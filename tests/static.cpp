// Static files: disk and #embed, through LocalClient (the same pipeline as the
// engine, no sockets).

#include <crocket/crocket.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace crocket;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) {                                                                    \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    ++g_checks;                                                                       \
    auto va_ = (a);                                                                   \
    auto vb_ = (b);                                                                   \
    if (!(va_ == vb_)) {                                                              \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (got %s)\n", __FILE__, __LINE__,  \
                   #a, #b, std::format("{}", va_).c_str());                           \
    }                                                                                 \
  } while (0)
static void section(const char* name) { std::fprintf(stderr, "[%s]\n", name); }

static constexpr unsigned char kHello[] = {
#embed "embed_hello.txt"
};
static constexpr unsigned char kNul[] = {
#embed "embed_nul.bin"
};
static_assert(sizeof kNul == 4);
static_assert(kNul[1] == 0);

namespace api {

[[= http::get("/api/ping")]]
auto ping() -> std::string { return "pong"; }

[[= http::get("/same")]]
auto same() -> std::string { return std::string(reinterpret_cast<const char*>(kHello), sizeof kHello); }

[[= http::post("/only")]]
auto post_only() -> std::string { return "posted"; }

[[= http::get("/static/owned.txt")]]
auto owned() -> std::string { return "from-route"; }

}  // namespace api

std::string hdr(const Response& r, std::string_view name) {
  auto v = r.headers.get(name);
  return v ? std::string(*v) : std::string();
}

struct Tmp {
  std::filesystem::path dir;
  std::filesystem::path outside;
  Tmp() {
    std::string pat = (std::filesystem::temp_directory_path() / "crocket-static-XXXXXX").string();
    if (!::mkdtemp(pat.data())) throw std::runtime_error("mkdtemp");
    dir = pat;
    outside = std::filesystem::path(pat + "-out");
    std::filesystem::create_directory(outside);
    write_at(outside / "secret.txt", "OUTSIDE_SECRET_MARKER");
  }
  ~Tmp() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::remove_all(outside, ec);
  }
  std::filesystem::path write(const std::string& rel, std::string_view body) {
    auto p = dir / rel;
    std::filesystem::create_directories(p.parent_path());
    write_at(p, body);
    return p;
  }
  static void write_at(const std::filesystem::path& p, std::string_view body) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
};

void set_mtime(const std::filesystem::path& p, time_t sec, long nsec) {
  timespec ts[2] = {};
  ts[0].tv_sec = sec;
  ts[0].tv_nsec = nsec;
  ts[1].tv_sec = sec;
  ts[1].tv_nsec = nsec;
  CHECK(::utimensat(AT_FDCWD, p.c_str(), ts, 0) == 0);
}

std::string ignite_msg(StaticFiles spec) {
  Crocket app;
  app.files(std::move(spec));
  auto r = app.ignite();
  return r ? std::string() : r.error().message();
}

void test_disk() {
  section("disk");
  Tmp tmp;
  tmp.write("index.html", "<h1>home</h1>\n");
  tmp.write("app.js", "console.log(1)\n");
  tmp.write("style.css", "body{}\n");
  tmp.write("data.json", "{\"a\":1}\n");
  tmp.write("pic.png", "not-a-png");
  tmp.write("noext", "raw");
  tmp.write("Index.HTML", "<b>caps</b>");
  tmp.write("ok.txt", "abcdefghijklmnopqrstuvwxyz");
  tmp.write("sub/a.txt", "nested");
  tmp.write("sub/index.html", "subindex");
  tmp.write("docs/index.html", "docsindex");
  tmp.write(".env", "DOTENV");
  tmp.write("sub/.secret", "NESTED_DOT");
  tmp.write("healthz", "NOT_HEALTH");
  tmp.write("owned.txt", "FILE_BYTES");
  tmp.write("only", "FILE_ONLY");
  tmp.write("empty.txt", "");
  tmp.write("tune.weba", "weba");
  std::filesystem::create_symlink("ok.txt", tmp.dir / "alias.txt");
  std::filesystem::create_symlink("sub", tmp.dir / "sublink");
  std::filesystem::create_symlink("../ok.txt", tmp.dir / "sub" / "back.txt");
  std::filesystem::create_symlink(tmp.outside / "secret.txt", tmp.dir / "abs-secret");
  auto rel_out = std::filesystem::path("..") / tmp.outside.filename() / "secret.txt";
  std::filesystem::create_symlink(rel_out, tmp.dir / "escape");
  std::filesystem::create_symlink(tmp.outside, tmp.dir / "outlink");

  Crocket app;
  app.files({.prefix = "/static",
             .root = tmp.dir.string(),
             .types = {{"weba", "audio/webm"}}})
      .mount("/", reflect_routes<^^api>());
  LocalClient c(app);

  auto js = c.get("/static/app.js").dispatch();
  CHECK_EQ(js.status, 200);
  CHECK_EQ(js.body, "console.log(1)\n");
  CHECK_EQ(hdr(js, "content-type"), "text/javascript; charset=utf-8");
  CHECK_EQ(hdr(js, "cache-control"), "no-cache");
  CHECK(hdr(js, "etag").starts_with("\""));
  CHECK(!hdr(js, "etag").starts_with("W/"));
  CHECK(!hdr(js, "last-modified").empty());
  CHECK_EQ(hdr(js, "accept-ranges"), "bytes");
  CHECK_EQ(hdr(js, "x-content-type-options"), "nosniff");
  CHECK(hdr(js, "content-security-policy").find("default-src") != std::string::npos);
  CHECK(!hdr(js, "x-request-id").empty());

  auto css = c.get("/static/style.css").dispatch();
  CHECK_EQ(hdr(css, "content-type"), "text/css; charset=utf-8");
  auto json = c.get("/static/data.json").dispatch();
  CHECK_EQ(hdr(json, "content-type"), "application/json");
  CHECK(hdr(json, "content-type").find("charset") == std::string::npos);
  auto png = c.get("/static/pic.png").dispatch();
  CHECK_EQ(hdr(png, "content-type"), "image/png");
  CHECK_EQ(hdr(c.get("/static/noext").dispatch(), "content-type"), "application/octet-stream");
  CHECK_EQ(hdr(c.get("/static/Index.HTML").dispatch(), "content-type"), "text/html; charset=utf-8");
  CHECK_EQ(hdr(c.get("/static/tune.weba").dispatch(), "content-type"), "audio/webm");
  CHECK_EQ(c.get("/static/empty.txt").dispatch().body, "");
  CHECK(!hdr(c.get("/static/empty.txt").dispatch(), "etag").empty());

  CHECK_EQ(c.get("/static/sub/a.txt").dispatch().body, "nested");
  CHECK_EQ(c.get("/static/./ok.txt").dispatch().body, "abcdefghijklmnopqrstuvwxyz");
  CHECK_EQ(c.get("/app.js").dispatch().status, 404);
  CHECK_EQ(c.get("/staticfile/app.js").dispatch().status, 404);
  CHECK(c.get("/static/missing.txt").dispatch().body.find("not_found") != std::string::npos);
  CHECK(c.get("/static/missing.txt").dispatch().body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);

  auto root = c.get("/static/").dispatch();
  CHECK_EQ(root.status, 200);
  CHECK_EQ(root.body, "<h1>home</h1>\n");
  CHECK_EQ(hdr(root, "content-type"), "text/html; charset=utf-8");
  auto slash = c.get("/static").dispatch();
  CHECK_EQ(slash.status, 308);
  CHECK_EQ(hdr(slash, "location"), "/static/");
  CHECK(slash.body.empty());
  auto sub = c.get("/static/sub").dispatch();
  CHECK_EQ(sub.status, 308);
  CHECK_EQ(hdr(sub, "location"), "/static/sub/");
  CHECK_EQ(c.get("/static/sub/").dispatch().body, "subindex");
  CHECK_EQ(c.get("/static/docs/").dispatch().body, "docsindex");

  CHECK_EQ(c.get("/static/.env").dispatch().status, 404);
  CHECK(c.get("/static/.env").dispatch().body.find("DOTENV") == std::string::npos);
  CHECK_EQ(c.get("/static/%2eenv").dispatch().status, 404);
  CHECK_EQ(c.get("/static/sub/.secret").dispatch().status, 404);
  CHECK(c.get("/static/sub/.secret").dispatch().body.find("NESTED_DOT") == std::string::npos);

  CHECK_EQ(c.get("/static/alias.txt").dispatch().body, "abcdefghijklmnopqrstuvwxyz");
  CHECK_EQ(c.get("/static/sublink/a.txt").dispatch().body, "nested");
  CHECK_EQ(c.get("/static/sub/back.txt").dispatch().body, "abcdefghijklmnopqrstuvwxyz");
  auto escaped = c.get("/static/escape").dispatch();
  CHECK_EQ(escaped.status, 404);
  CHECK(escaped.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  auto abs = c.get("/static/abs-secret").dispatch();
  CHECK_EQ(abs.status, 404);
  CHECK(abs.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  auto outlink = c.get("/static/outlink/secret.txt").dispatch();
  CHECK_EQ(outlink.status, 404);
  CHECK(outlink.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);

  auto climb = c.get("/static/../" + tmp.outside.filename().string() + "/secret.txt").dispatch();
  CHECK_EQ(climb.status, 404);
  CHECK(climb.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  auto encoded = c.get("/static/%2e%2e/" + tmp.outside.filename().string() + "/secret.txt").dispatch();
  CHECK_EQ(encoded.status, 404);
  CHECK(encoded.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  auto doubled = c.get("/static/%252e%252e/secret.txt").dispatch();
  CHECK_EQ(doubled.status, 404);
  CHECK(doubled.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  // A path capture decodes the normalized segment again, so %2e%2e%2f is "../"
  // and %2e%2e%5c is "..\\". Joining either onto the root would leave it.
  auto outside_name = tmp.outside.filename().string();
  for (const char* enc : {"%2e%2e%2f", "%2E%2E%2F", "%2e%2e%5c", "%2e%2e%2f%2e%2e%2f", "%252e%252e%252f"}) {
    auto hit = c.get(std::string("/static/") + enc + outside_name + "/secret.txt").dispatch();
    CHECK_EQ(hit.status, 404);
    CHECK(hit.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  }
  CHECK_EQ(c.get("/static/sub%2Fa.txt").dispatch().status, 404);
  CHECK(c.get("/static/sub%2Fa.txt").dispatch().body.find("nested") == std::string::npos);
  tmp.write("a b.txt", "spaced");
  tmp.write("100%.txt", "percent");
  CHECK_EQ(c.get("/static/a%20b.txt").dispatch().body, "spaced");
  CHECK_EQ(c.get("/static/100%25.txt").dispatch().body, "percent");
  CHECK_EQ(c.get("/static//ok.txt").dispatch().status, 404);
  CHECK_EQ(c.get("/static/%00").dispatch().status, 400);

  auto health = c.get("/healthz").dispatch();
  CHECK_EQ(health.status, 200);
  CHECK_EQ(health.body, "{\"status\":\"ok\"}");
  CHECK(health.body.find("NOT_HEALTH") == std::string::npos);
  CHECK_EQ(c.get("/static/owned.txt").dispatch().body, "from-route");
  {
    Tmp root;
    root.write("only", "FILE_ONLY");
    Crocket owned;
    owned.files({.prefix = "/", .root = root.dir.string()}).mount("/", reflect_routes<^^api>());
    LocalClient oc(owned);
    auto method = oc.get("/only").dispatch();
    CHECK_EQ(method.status, 405);
    CHECK(method.body.find("FILE_ONLY") == std::string::npos);
  }
  CHECK_EQ(c.post("/static/ok.txt").dispatch().status, 404);
  CHECK(c.post("/static/ok.txt").dispatch().body.find("abcdefghijklmnopqrstuvwxyz") == std::string::npos);
  CHECK_EQ(c.get("/api/ping").dispatch().body, "pong");

  auto got = c.get("/static/ok.txt").dispatch();
  auto head = c.head("/static/ok.txt").dispatch();
  CHECK_EQ(head.status, 200);
  CHECK_EQ(head.body, got.body);  // LocalClient keeps it; the engine sends none
  CHECK_EQ(hdr(head, "etag"), hdr(got, "etag"));
  CHECK_EQ(hdr(head, "content-type"), hdr(got, "content-type"));
  CHECK_EQ(hdr(head, "last-modified"), hdr(got, "last-modified"));

  auto etag = hdr(got, "etag");
  auto modified = hdr(got, "last-modified");
  auto none = c.get("/static/ok.txt").header("if-none-match", etag).dispatch();
  CHECK_EQ(none.status, 304);
  CHECK(none.body.empty());
  CHECK_EQ(hdr(none, "etag"), etag);
  auto weak = c.get("/static/ok.txt").header("if-none-match", "W/" + etag).dispatch();
  CHECK_EQ(weak.status, 304);
  auto star = c.get("/static/ok.txt").header("if-none-match", "*").dispatch();
  CHECK_EQ(star.status, 304);
  auto listed = c.get("/static/ok.txt").header("if-none-match", "\"nope\", " + etag).dispatch();
  CHECK_EQ(listed.status, 304);
  auto other = c.get("/static/ok.txt").header("if-none-match", "\"nope\"").dispatch();
  CHECK_EQ(other.status, 200);
  CHECK_EQ(other.body, got.body);
  auto fresh = c.get("/static/ok.txt").header("if-modified-since", modified).dispatch();
  CHECK_EQ(fresh.status, 304);
  auto stale = c.get("/static/ok.txt").header("if-modified-since", "Sun, 06 Nov 1994 08:49:37 GMT").dispatch();
  CHECK_EQ(stale.status, 200);
  auto inm_wins = c.get("/static/ok.txt")
                      .header("if-none-match", "\"nope\"")
                      .header("if-modified-since", modified)
                      .dispatch();
  CHECK_EQ(inm_wins.status, 200);
  auto head_304 = c.head("/static/ok.txt").header("if-none-match", etag).dispatch();
  CHECK_EQ(head_304.status, 304);
  CHECK(head_304.body.empty());

  auto part = c.get("/static/ok.txt").header("range", "bytes=0-3").dispatch();
  CHECK_EQ(part.status, 206);
  CHECK_EQ(part.body, "abcd");
  CHECK_EQ(hdr(part, "content-range"), "bytes 0-3/26");
  auto tail = c.get("/static/ok.txt").header("range", "bytes=-4").dispatch();
  CHECK_EQ(tail.status, 206);
  CHECK_EQ(tail.body, "wxyz");
  auto bad_range = c.get("/static/ok.txt").header("range", "bytes=100-200").dispatch();
  CHECK_EQ(bad_range.status, 416);
  CHECK(hdr(bad_range, "content-range").find("bytes */") != std::string::npos);
  auto multi = c.get("/static/ok.txt").header("range", "bytes=0-1,2-3").dispatch();
  CHECK_EQ(multi.status, 200);
  CHECK_EQ(multi.body, got.body);
  auto if_range = c.get("/static/ok.txt").header("range", "bytes=0-3").header("if-range", etag).dispatch();
  CHECK_EQ(if_range.status, 206);
  CHECK_EQ(if_range.body, "abcd");
  auto if_range_miss =
      c.get("/static/ok.txt").header("range", "bytes=0-3").header("if-range", "\"nope\"").dispatch();
  CHECK_EQ(if_range_miss.status, 200);
  CHECK_EQ(if_range_miss.body, got.body);

  auto p = tmp.write("shift.txt", "aaaa");
  set_mtime(p, 1'700'000'000, 10);
  auto e1 = hdr(c.get("/static/shift.txt").dispatch(), "etag");
  tmp.write("shift.txt", "bbbb");
  set_mtime(p, 1'700'000'000, 10);
  auto same_meta = c.get("/static/shift.txt").dispatch();
  CHECK_EQ(same_meta.body, "bbbb");
  CHECK_EQ(hdr(same_meta, "etag"), e1);
  set_mtime(p, 1'700'000'000, 11);
  auto moved = c.get("/static/shift.txt").dispatch();
  CHECK(hdr(moved, "etag") != e1);
  CHECK_EQ(moved.body, "bbbb");

  tmp.write("later.txt", "after-ignite");
  CHECK_EQ(c.get("/static/later.txt").dispatch().body, "after-ignite");
}

void test_dotfiles_cache_and_mounts() {
  section("options");
  Tmp web;
  web.write(".env", "DOTENV");
  web.write("ok.txt", "ok");
  web.write("index.html", "idx");
  Tmp other;
  other.write("static/leaked.txt", "LEAKED");
  other.write("static/nope.txt", "LEAKED_NOPE");
  other.write("ok.txt", "from-root");
  Tmp stat;
  stat.write("other.txt", "from-static");
  stat.write("index.html", "static-index");

  {
    Crocket app;
    app.files({.prefix = "/", .root = web.dir.string(), .dotfiles = true, .cache_control = "public, max-age=60"});
    LocalClient c(app);
    CHECK_EQ(c.get("/.env").dispatch().body, "DOTENV");
    CHECK_EQ(hdr(c.get("/ok.txt").dispatch(), "cache-control"), "public, max-age=60");
  }
  {
    Crocket app;
    app.files({.prefix = "/", .root = other.dir.string()});
    app.files({.prefix = "/static", .root = stat.dir.string()});
    LocalClient c(app);
    CHECK_EQ(c.get("/static/other.txt").dispatch().body, "from-static");
    CHECK_EQ(c.get("/ok.txt").dispatch().body, "from-root");
    auto nope = c.get("/static/nope.txt").dispatch();
    CHECK_EQ(nope.status, 404);
    CHECK(nope.body.find("LEAKED") == std::string::npos);
    CHECK_EQ(c.get("/static/").dispatch().body, "static-index");
  }
  {
    Tmp bare;
    bare.write("sub/a.txt", "a");
    bare.write("note.txt", "note");
    Crocket app;
    app.files({.prefix = "/", .root = bare.dir.string(), .index = ""});
    LocalClient c(app);
    CHECK_EQ(c.get("/").dispatch().status, 404);
    CHECK_EQ(c.get("/sub").dispatch().status, 404);
    CHECK_EQ(c.get("/sub/").dispatch().status, 404);
    CHECK_EQ(c.get("/note.txt").dispatch().body, "note");
  }
  {
    Tmp big;
    big.write("tiny.txt", "abcd");
    big.write("big.txt", "abcde");
    Crocket app;
    app.files({.prefix = "/", .root = big.dir.string(), .max_bytes = 4});
    LocalClient c(app);
    CHECK_EQ(c.get("/tiny.txt").dispatch().status, 200);
    auto over = c.get("/big.txt").dispatch();
    CHECK_EQ(over.status, 500);
    CHECK(over.body.find("static.too_large") != std::string::npos);
  }
}

void test_spa_and_middleware() {
  section("spa");
  Tmp tmp;
  tmp.write("index.html", "SPA_INDEX");
  tmp.write("app.js", "js");
  tmp.write(".env", "DOTENV");
  Config cfg;
  cfg.proxy.allowed_hosts = {"app.test"};
  Crocket app(cfg);
  app.attach(Cors::allow_origins({"https://app.example.com"}))
      .attach(Metrics{"/metrics"})
      .files({.prefix = "/", .root = tmp.dir.string(), .spa = true})
      .mount("/", reflect_routes<^^api>());
  LocalClient c(app);

  auto page = c.get("/users/42").header("host", "app.test").dispatch();
  CHECK_EQ(page.status, 200);
  CHECK_EQ(page.body, "SPA_INDEX");
  CHECK_EQ(hdr(page, "content-type"), "text/html; charset=utf-8");
  CHECK_EQ(c.get("/missing.js").header("host", "app.test").dispatch().status, 404);
  auto dot = c.get("/.env").header("host", "app.test").dispatch();
  CHECK_EQ(dot.status, 404);
  CHECK(dot.body.find("SPA_INDEX") == std::string::npos);
  CHECK(dot.body.find("DOTENV") == std::string::npos);
  CHECK_EQ(c.get("/api/ping").header("host", "app.test").dispatch().body, "pong");
  CHECK_EQ(c.get("/healthz").header("host", "app.test").dispatch().body, "{\"status\":\"ok\"}");
  CHECK_EQ(c.get("/app.js").header("host", "app.test").dispatch().body, "js");

  auto denied = c.get("/app.js").header("host", "evil.test").dispatch();
  CHECK_EQ(denied.status, 400);
  CHECK(denied.body.find("host.invalid") != std::string::npos);
  CHECK(denied.body.find("\njs") == std::string::npos);

  auto cors = c.get("/app.js").header("host", "app.test").header("origin", "https://app.example.com").dispatch();
  CHECK_EQ(hdr(cors, "access-control-allow-origin"), "https://app.example.com");

  c.get("/app.js").header("host", "app.test").dispatch();
  c.get("/no-such-file").header("host", "app.test").dispatch();  // spa, still the static label
  auto metrics = c.get("/metrics").header("host", "app.test").dispatch();
  CHECK(metrics.body.find("route=\"/{path}\"") != std::string::npos);
  CHECK(metrics.body.find("route=\"/api/ping\"") != std::string::npos);
  CHECK(metrics.body.find("app.js") == std::string::npos);
}

void test_embed() {
  section("embed");
  Crocket app;
  app.files({.prefix = "/embed",
             .assets = {asset("hello.txt", kHello), asset("js/app.js", kNul), asset("index.html", kHello),
                        asset("sub/index.html", kHello)}})
      .mount("/", reflect_routes<^^api>());
  LocalClient c(app);
  auto hello = c.get("/embed/hello.txt").dispatch();
  CHECK_EQ(hello.status, 200);
  CHECK_EQ(hello.body, std::string(reinterpret_cast<const char*>(kHello), sizeof kHello));
  CHECK_EQ(hdr(hello, "content-type"), "text/plain; charset=utf-8");
  CHECK(hdr(hello, "last-modified").empty());
  auto etag = hdr(hello, "etag");
  CHECK_EQ(etag.size(), 18u);
  CHECK(etag.starts_with("\""));
  CHECK(!etag.starts_with("W/"));
  CHECK_EQ(hdr(c.get("/embed/hello.txt").dispatch(), "etag"), etag);
  CHECK_EQ(hdr(c.get("/same").dispatch(), "etag"), etag);
  auto none = c.get("/embed/hello.txt").header("if-none-match", etag).dispatch();
  CHECK_EQ(none.status, 304);
  CHECK(none.body.empty());

  auto bin = c.get("/embed/js/app.js").dispatch();
  CHECK_EQ(bin.status, 200);
  CHECK_EQ(bin.body.size(), 4u);
  CHECK_EQ(bin.body[0], 'a');
  CHECK_EQ(bin.body[1], '\0');
  CHECK_EQ(bin.body[2], 'b');
  CHECK_EQ(hdr(bin, "content-type"), "text/javascript; charset=utf-8");

  CHECK_EQ(c.get("/embed/").dispatch().status, 200);
  CHECK_EQ(c.get("/embed").dispatch().status, 308);
  CHECK_EQ(hdr(c.get("/embed").dispatch(), "location"), "/embed/");
  CHECK_EQ(c.get("/embed/sub").dispatch().status, 308);
  CHECK_EQ(c.get("/embed/sub/").dispatch().status, 200);
  CHECK_EQ(c.get("/embed/nope.txt").dispatch().status, 404);
  CHECK_EQ(c.get("/embed/../hello.txt").dispatch().status, 404);
  auto slipped = c.get("/embed/%2e%2e%2fhello.txt").dispatch();
  CHECK_EQ(slipped.status, 404);
  CHECK(slipped.body.find("embedded-hello") == std::string::npos);
  auto slipped_up = c.get("/embed/%2E%2E%5Cjs%2Fapp.js").dispatch();
  CHECK_EQ(slipped_up.status, 404);

  auto part = c.get("/embed/hello.txt").header("range", "bytes=0-3").dispatch();
  CHECK_EQ(part.status, 206);
  CHECK_EQ(part.body, hello.body.substr(0, 4));
}

void test_encoded_traversal() {
  section("encoded traversal");
  Tmp tmp;
  tmp.write("ok.txt", "inside");
  tmp.write("dir/a.txt", "inside-dir");
  Crocket app;
  // Prefix "/" so the whole remainder is the relative path. A handler that
  // took this as a capture and appended it to the root would read the secret.
  app.files({.prefix = "/", .root = tmp.dir.string()});
  LocalClient c(app);
  auto outside_name = tmp.outside.filename().string();
  for (const char* enc : {"%2e%2e%2f", "%2E%2E%2F", "%2e%2e%5c", "%2e%2e%2f%2e%2e%2f", "%252e%252e%252f"}) {
    auto hit = c.get(std::string("/") + enc + outside_name + "/secret.txt").dispatch();
    CHECK_EQ(hit.status, 404);
    CHECK(hit.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  }
  auto dotdot = c.get("/../" + outside_name + "/secret.txt").dispatch();
  CHECK_EQ(dotdot.status, 404);
  CHECK(dotdot.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
  CHECK_EQ(c.get("/%2e%2e%00ok.txt").dispatch().status, 400);
  CHECK_EQ(c.get("/ok.txt").dispatch().body, "inside");
  CHECK_EQ(c.get("/dir/a.txt").dispatch().body, "inside-dir");
  auto slash = c.get("/dir%2Fa.txt").dispatch();
  CHECK_EQ(slash.status, 404);
  CHECK(slash.body.find("inside-dir") == std::string::npos);
  auto both = c.get("/dir%2f%2e%2e%2f" + outside_name + "/secret.txt").dispatch();
  CHECK_EQ(both.status, 404);
  CHECK(both.body.find("OUTSIDE_SECRET_MARKER") == std::string::npos);
}

void test_spa_embed_and_ignite() {
  section("ignite");
  Tmp tmp;
  tmp.write("index.html", "SPA_INDEX");
  tmp.write("app.js", "js");
  {
    Crocket app;
    app.files({.prefix = "/", .assets = {asset("index.html", kHello), asset("app.js", kNul)}, .spa = true});
    LocalClient c(app);
    CHECK_EQ(c.get("/users/1").dispatch().body, std::string(reinterpret_cast<const char*>(kHello), sizeof kHello));
    CHECK_EQ(c.get("/missing.js").dispatch().status, 404);
  }
  {
    Crocket app;
    app.files({.prefix = "/", .root = tmp.dir.string()});
    CHECK(app.ignite().has_value());
    app.files({.prefix = "/other", .root = tmp.dir.string()});
    LocalClient c(app);
    CHECK_EQ(c.get("/index.html").dispatch().status, 200);
    CHECK_EQ(c.get("/other/index.html").dispatch().status, 404);
  }

  Tmp missing;
  CHECK(ignite_msg({.root = (missing.dir / "nope").string()}).find("not a directory") != std::string::npos);
  auto file_root = tmp.write("notdir", "x");
  CHECK(ignite_msg({.root = file_root.string()}).find("not a directory") != std::string::npos);
  CHECK(ignite_msg({.root = tmp.dir.string(), .assets = {asset("a.txt", kHello)}}).find("not both") != std::string::npos);
  CHECK(ignite_msg({}).find("set root or assets") != std::string::npos);
  CHECK(ignite_msg({.prefix = "static", .root = tmp.dir.string()}).find("must start with '/'") != std::string::npos);
  CHECK(ignite_msg({.prefix = "/static/../x", .root = tmp.dir.string()}).find("..") != std::string::npos);
  CHECK(ignite_msg({.assets = {asset("../x.txt", kHello)}}).find("..") != std::string::npos);
  CHECK(ignite_msg({.assets = {asset("a.txt", kHello), asset("/a.txt", kHello)}}).find("duplicate") != std::string::npos);
  CHECK(ignite_msg({.assets = {asset(".env", kHello)}}).find("dotfile") != std::string::npos);
  CHECK(ignite_msg({.root = tmp.dir.string(), .index = "", .spa = true}).find("spa needs") != std::string::npos);
  CHECK(ignite_msg({.assets = {asset("app.js", kHello)}, .spa = true}).find("spa index") != std::string::npos);
  Tmp no_index;
  no_index.write("app.js", "js");
  CHECK(ignite_msg({.root = no_index.dir.string(), .spa = true}).find("spa index") != std::string::npos);
  CHECK(ignite_msg({.root = tmp.dir.string(), .cache_control = "no-cache\r\nX-Injected: 1"}).find("cache-control") !=
        std::string::npos);
  CHECK(ignite_msg({.root = tmp.dir.string(), .index = "a/b.html"}).find("single file name") != std::string::npos);
  CHECK(ignite_msg({.assets = {asset("a.txt", kHello)}, .max_bytes = 4}).find("max_bytes") != std::string::npos);
  {
    Crocket app;
    app.files({.prefix = "/", .assets = {asset(".env", kHello)}, .dotfiles = true});
    LocalClient c(app);
    CHECK_EQ(c.get("/.env").dispatch().status, 200);
  }
}

int main() {
  test_disk();
  test_dotfiles_cache_and_mounts();
  test_spa_and_middleware();
  test_embed();
  test_encoded_traversal();
  test_spa_embed_and_ignite();
  std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
