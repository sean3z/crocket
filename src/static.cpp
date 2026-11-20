#include "static.hpp"

#include "conditional.hpp"
#include "crocket/responder.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace crocket::detail {

struct FileBytes {
  std::string path;
  std::span<const std::byte> bytes;
  std::string etag;
  std::string type;
};

struct Mount {
  std::string prefix;
  std::string label;
  std::string index;
  bool dotfiles = false;
  bool spa = false;
  std::string cache_control;
  bool precompressed = false;
  bool fallthrough = false;
  std::vector<CacheRule> cache;
  std::vector<HeaderRule> headers;
  std::size_t max_bytes = 0;
  std::vector<std::pair<std::string, std::string>> types;
  int root_fd = -1;
  std::string root_real;
  std::vector<FileBytes> files;

  Mount() = default;
  Mount(const Mount&) = delete;
  Mount& operator=(const Mount&) = delete;
  Mount(Mount&& o) noexcept
      : prefix(std::move(o.prefix)),
        label(std::move(o.label)),
        index(std::move(o.index)),
        dotfiles(o.dotfiles),
        spa(o.spa),
        cache_control(std::move(o.cache_control)),
        precompressed(o.precompressed),
        fallthrough(o.fallthrough),
        cache(std::move(o.cache)),
        headers(std::move(o.headers)),
        max_bytes(o.max_bytes),
        types(std::move(o.types)),
        root_fd(std::exchange(o.root_fd, -1)),
        root_real(std::move(o.root_real)),
        files(std::move(o.files)) {}
  Mount& operator=(Mount&&) = delete;
  ~Mount() {
    if (root_fd >= 0) ::close(root_fd);
  }
};

struct Fd {
  int fd = -1;
  explicit Fd(int f = -1) noexcept : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      if (fd >= 0) ::close(fd);
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  [[nodiscard]] int get() const noexcept { return fd; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd >= 0; }
};

namespace {

constexpr int kOpen = O_RDONLY | O_CLOEXEC | O_NOCTTY;

constexpr std::pair<std::string_view, std::string_view> kMime[] = {
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"cjs", "text/javascript; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"eot", "application/vnd.ms-fontobject"},
    {"gif", "image/gif"},
    {"gz", "application/gzip"},
    {"htm", "text/html; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"ico", "image/x-icon"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"markdown", "text/markdown; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"ogg", "audio/ogg"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain; charset=utf-8"},
    {"vtt", "text/vtt; charset=utf-8"},
    {"wasm", "application/wasm"},
    {"wav", "audio/wav"},
    {"webm", "video/webm"},
    {"webmanifest", "application/manifest+json"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"xml", "application/xml"},
    {"zip", "application/zip"},
};

std::string errno_text(int err) { return std::generic_category().message(err); }

std::string lower_copy(std::string_view s) {
  std::string o(s);
  for (auto& c : o)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return o;
}

/// Extension of the last segment, without the dot. ".env" and "noext" have none.
std::string extension_of(std::string_view path) {
  auto slash = path.rfind('/');
  auto base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  auto dot = base.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == base.size()) return {};
  return lower_copy(base.substr(dot + 1));
}

std::string mime_of(const Mount& m, std::string_view path) {
  auto ext = extension_of(path);
  if (ext.empty()) return "application/octet-stream";
  for (auto& [e, t] : m.types)
    if (e == ext) return t;
  for (auto& [e, t] : kMime)
    if (e == ext) return std::string(t);
  return "application/octet-stream";
}

bool inside_root(std::string_view root, std::string_view path) {
  if (root.empty() || path.empty()) return false;
  if (path == root) return true;
  if (root == "/") return path.front() == '/';
  return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
}

/// The open file's canonical path. False when it can't be read or it sits
/// outside `root`. The fd stays the one the caller will read.
bool fd_inside(int fd, const std::string& root) {
  char link[64];
  std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  char resolved[PATH_MAX];
  if (!::realpath(link, resolved)) return false;
  return inside_root(root, resolved);
}

// A normal entry is opened with O_NOFOLLOW, so ".." can't be smuggled in and a
// symlink can't be followed by accident. A symlink (ELOOP) is opened and kept
// only when its canonical path stays inside the root. h2o's file handler
// follows every symlink and answers before crocket's pipeline, so it isn't used.
enum class Opened { Ok, Missing, Blocked, Busy };

struct OpenResult {
  Opened kind = Opened::Missing;
  Fd fd;
};

OpenResult open_checked(int dir, const std::string& name, const std::string& root) {
  // `name` is one component, already decoded. A slash here would make openat
  // walk out of `dir`, so it is a miss rather than a join.
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
    return {Opened::Blocked, Fd{}};
  int fd = ::openat(dir, name.c_str(), kOpen | O_NOFOLLOW);
  if (fd < 0 && errno == ELOOP) {
    fd = ::openat(dir, name.c_str(), kOpen);
    if (fd < 0) return {errno == EMFILE || errno == ENFILE ? Opened::Busy : Opened::Missing, Fd{}};
    if (!fd_inside(fd, root)) {
      ::close(fd);
      return {Opened::Blocked, Fd{}};
    }
    return {Opened::Ok, Fd{fd}};
  }
  if (fd < 0) return {errno == EMFILE || errno == ENFILE ? Opened::Busy : Opened::Missing, Fd{}};
  return {Opened::Ok, Fd{fd}};
}

/// The file name for one normalized segment. A path capture percent-decodes
/// that segment, and `%2e%2e%2f` becomes "../", which contains a slash. That
/// decoded string is never appended to the root: if it isn't a single safe
/// component, the request is a miss.
std::optional<std::string> component_name(std::string_view raw) {
  auto name = percent_decode(raw, false);
  if (name.empty() || name == "." || name == "..") return std::nullopt;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find('\0') != std::string::npos)
    return std::nullopt;
  return name;
}

struct Rel {
  bool blocked = false;
  bool dir = false;
  std::vector<std::string> segs;
};

Rel split_rel(std::string_view rel, bool dotfiles) {
  Rel out;
  if (rel.empty()) {
    out.dir = true;
    return out;
  }
  if (rel.front() == '/' || rel.find('\\') != std::string_view::npos || rel.find('\0') != std::string_view::npos) {
    out.blocked = true;
    return out;
  }
  std::size_t i = 0;
  while (i <= rel.size()) {
    auto slash = rel.find('/', i);
    bool last = slash == std::string_view::npos;
    auto seg = last ? rel.substr(i) : rel.substr(i, slash - i);
    if (seg.empty()) {
      if (last) out.dir = true;
      else out.blocked = true;
    } else if (auto name = component_name(seg)) {
      if (name->front() == '.' && !dotfiles) out.blocked = true;
      else out.segs.push_back(std::move(*name));
    } else {
      out.blocked = true;
    }
    if (out.blocked || last) break;
    i = slash + 1;
    if (i > rel.size()) break;
    if (i == rel.size()) {
      out.dir = true;
      break;
    }
  }
  return out;
}

bool strip_prefix(const Mount& m, std::string_view path, std::string& rel) {
  if (m.prefix == "/") {
    if (path.empty() || path.front() != '/') return false;
    rel.assign(path.size() == 1 ? std::string_view{} : path.substr(1));
    return true;
  }
  if (path == m.prefix) {
    rel.clear();
    return true;
  }
  if (path.size() > m.prefix.size() && path.starts_with(m.prefix) && path[m.prefix.size()] == '/') {
    rel.assign(path.substr(m.prefix.size() + 1));
    return true;
  }
  return false;
}

bool prefix_matches(const Mount& m, std::string_view path) {
  if (m.prefix == "/") return !path.empty() && path.front() == '/';
  if (path == m.prefix) return true;
  return path.size() > m.prefix.size() && path.starts_with(m.prefix) && path[m.prefix.size()] == '/';
}

void label(Request& req, const Mount& m) {
  req.route_template = m.label;
  req.handler = "crocket::files";
  req.route = nullptr;
}

void redirect_to_slash(const Mount& m, Request& req, Response& res) {
  label(req, m);
  res.status = 308;
  res.headers.set("location", std::string(req.path) + "/");
  res.headers.set("cache-control", m.cache_control);
  res.body.clear();
}

std::string disk_etag(const struct stat& st) {
  auto sec = static_cast<unsigned long long>(st.st_mtim.tv_sec);
  auto nsec = static_cast<unsigned long long>(st.st_mtim.tv_nsec);
  auto sz = static_cast<unsigned long long>(st.st_size < 0 ? 0 : st.st_size);
  return std::format("\"{:x}.{:x}-{:x}\"", sec, nsec, sz);
}

/// `*` matches any run of characters except `/`. The whole relative path has to match.
bool glob_match(std::string_view pat, std::string_view text) {
  std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = t;
    } else if (p < pat.size() && pat[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos && text[mark] != '/') {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

/// q is missing or positive. q=0 rejects the coding.
bool q_accepts(std::string_view params) {
  while (!params.empty()) {
    auto semi = params.find(';');
    auto p = trim_ws(params.substr(0, semi));
    if (p.starts_with("q=")) {
      p.remove_prefix(2);
      double q = 0;
      auto r = std::from_chars(p.data(), p.data() + p.size(), q);
      return r.ec == std::errc() && q > 0;
    }
    if (semi == std::string_view::npos) break;
    params.remove_prefix(semi + 1);
  }
  return true;
}

/// br is preferred over gzip when both are acceptable. An absent header doesn't
/// select a compressed sibling.
void accept_encodings(std::string_view header, bool& br, bool& gzip) {
  br = gzip = false;
  if (header.empty()) return;
  bool star = false, br_no = false, gzip_no = false;
  while (!header.empty()) {
    auto comma = header.find(',');
    auto item = trim_ws(header.substr(0, comma));
    header = comma == std::string_view::npos ? std::string_view{} : header.substr(comma + 1);
    if (item.empty()) continue;
    auto semi = item.find(';');
    auto name = trim_ws(item.substr(0, semi));
    auto params = semi == std::string_view::npos ? std::string_view{} : item.substr(semi + 1);
    std::string lower(name);
    for (auto& c : lower)
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    bool ok = q_accepts(params);
    if (lower == "br") (ok ? br : br_no) = true;
    else if (lower == "gzip") (ok ? gzip : gzip_no) = true;
    else if (lower == "*") star = ok;
  }
  if (star && !br_no) br = true;
  if (star && !gzip_no) gzip = true;
}

std::string etag_for(std::string etag, std::string_view encoding) {
  if (encoding.empty() || etag.size() < 2 || etag.back() != '"') return etag;
  etag.insert(etag.size() - 1, "-");
  etag.insert(etag.size() - 1, encoding);
  return etag;
}

std::string cache_for(const Mount& m, std::string_view rel) {
  for (auto& rule : m.cache)
    if (glob_match(rule.glob, rel)) return rule.control;
  return m.cache_control;
}

void apply_file_headers(const Mount& m, std::string_view rel, Response& res) {
  for (auto& rule : m.headers) {
    if (!glob_match(rule.glob, rel)) continue;
    for (auto& [name, value] : rule.fields) res.headers.set(name, value);
    return;
  }
}

void finish_headers(const Mount& m, Response& res, std::string_view rel, std::string etag, std::string_view encoding) {
  res.headers.set("etag", etag_for(std::move(etag), encoding));
  res.headers.set("cache-control", cache_for(m, rel));
  if (encoding.empty()) res.headers.set("accept-ranges", "bytes");
  else res.headers.set("content-encoding", encoding);
  // A compressed body is not sliced: Range describes the decoded representation,
  // and we don't advertise ranges for the encoded one.
  if (m.precompressed) res.headers.set("vary", "accept-encoding");
  apply_file_headers(m, rel, res);
}

enum class Hit { Served, Miss, Blocked };

void fail(const Mount& m, Request& req, Response& res, ApiError err) {
  label(req, m);
  write_error(std::move(err), req, res);
}

Hit read_fd(const Mount& m, Request& req, Response& res, int fd, const struct stat& st, std::string_view relpath,
           std::string_view encoding) {
  if (!S_ISREG(st.st_mode)) return Hit::Miss;
  if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > m.max_bytes) {
    fail(m, req, res, {500, "static.too_large", "file is larger than the static file limit",
                       std::to_string(static_cast<unsigned long long>(st.st_size)) + " bytes"});
    return Hit::Served;
  }
  auto n = static_cast<std::size_t>(st.st_size);
  std::string body(n, '\0');
  std::size_t got = 0;
  while (got < n) {
    ssize_t r = ::pread(fd, body.data() + got, n - got, static_cast<off_t>(got));
    if (r < 0) {
      if (errno == EINTR) continue;
      fail(m, req, res, {500, "static.io", "could not read file", errno_text(errno)});
      return Hit::Served;
    }
    if (r == 0) break;
    got += static_cast<std::size_t>(r);
  }
  if (got != n) {
    fail(m, req, res, {500, "static.io", "could not read file", "short read"});
    return Hit::Served;
  }
  label(req, m);
  res.status = 200;
  res.set_content_type(mime_of(m, relpath));
  res.headers.set("last-modified", http::date(std::chrono::system_clock::from_time_t(st.st_mtim.tv_sec)));
  finish_headers(m, res, relpath, disk_etag(st), encoding);
  res.body = std::move(body);
  return Hit::Served;
}

/// A compressed sibling, or a skip when it isn't there. An error response is Served.
Hit read_variant(const Mount& m, Request& req, Response& res, int dir, const std::string& name, std::string_view relpath,
                 std::string_view encoding) {
  auto opened = open_checked(dir, name, m.root_real);
  if (opened.kind == Opened::Busy) {
    fail(m, req, res, {503, "server.busy", "server is at capacity", "too many open files"});
    return Hit::Served;
  }
  if (opened.kind != Opened::Ok) return Hit::Miss;
  struct stat st {};
  if (::fstat(opened.fd.get(), &st) != 0) {
    fail(m, req, res, {500, "static.io", "could not read file", errno_text(errno)});
    return Hit::Served;
  }
  if (!S_ISREG(st.st_mode)) return Hit::Miss;
  return read_fd(m, req, res, opened.fd.get(), st, relpath, encoding);
}

Hit serve_plain_or_encoded(const Mount& m, Request& req, Response& res, int dir, const std::string& name, int plain_fd,
                           const struct stat& plain_st, std::string_view relpath) {
  if (m.precompressed) {
    bool br = false, gzip = false;
    accept_encodings(req.header("accept-encoding").value_or(""), br, gzip);
    if (br) {
      auto hit = read_variant(m, req, res, dir, name + ".br", relpath, "br");
      if (hit == Hit::Served) return hit;
    }
    if (gzip) {
      auto hit = read_variant(m, req, res, dir, name + ".gz", relpath, "gzip");
      if (hit == Hit::Served) return hit;
    }
  }
  return read_fd(m, req, res, plain_fd, plain_st, relpath, {});
}

Hit serve_opened(const Mount& m, Request& req, Response& res, int dir, const std::string& name, Fd fd,
                std::string_view relpath) {
  struct stat st {};
  if (::fstat(fd.get(), &st) != 0) {
    fail(m, req, res, {500, "static.io", "could not read file", errno_text(errno)});
    return Hit::Served;
  }
  if (!S_ISREG(st.st_mode)) return Hit::Miss;
  return serve_plain_or_encoded(m, req, res, dir, name, fd.get(), st, relpath);
}

const FileBytes* find_file(const Mount& m, const std::string& key) {
  auto it = std::ranges::lower_bound(m.files, key, {}, &FileBytes::path);
  if (it != m.files.end() && it->path == key) return &*it;
  return nullptr;
}

bool has_child(const Mount& m, const std::string& dir) {
  auto prefix = dir.empty() ? std::string() : dir + "/";
  auto it = std::ranges::lower_bound(m.files, prefix, {}, &FileBytes::path);
  return it != m.files.end() && it->path.starts_with(prefix);
}

void serve_embedded(const Mount& m, Request& req, Response& res, const FileBytes& plain) {
  const FileBytes* body = &plain;
  std::string_view encoding;
  if (m.precompressed) {
    bool br = false, gzip = false;
    accept_encodings(req.header("accept-encoding").value_or(""), br, gzip);
    if (br)
      if (auto* v = find_file(m, plain.path + ".br")) {
        body = v;
        encoding = "br";
      }
    if (encoding.empty() && gzip)
      if (auto* v = find_file(m, plain.path + ".gz")) {
        body = v;
        encoding = "gzip";
      }
  }
  label(req, m);
  if (body->bytes.size() > m.max_bytes) {
    write_error({500, "static.too_large", "file is larger than the static file limit",
                 std::to_string(body->bytes.size()) + " bytes"},
                req, res);
    return;
  }
  res.status = 200;
  res.set_content_type(plain.type);
  finish_headers(m, res, plain.path, body->etag, encoding);
  res.body.assign(reinterpret_cast<const char*>(body->bytes.data()), body->bytes.size());
}

std::string join_segs(const std::vector<std::string>& segs) {
  std::string k;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i) k += '/';
    k += segs[i];
  }
  return k;
}

std::string index_key(const std::string& dir, const std::string& index) {
  return dir.empty() ? index : dir + "/" + index;
}

bool spa_eligible(const Rel& rel) {
  if (rel.blocked || rel.segs.empty()) return false;
  return rel.segs.back().find('.') == std::string::npos;
}

Hit serve_embed(const Mount& m, const Rel& rel, Request& req, Response& res) {
  if (rel.segs.empty()) {
    if (!req.path.ends_with('/')) {
      redirect_to_slash(m, req, res);
      return Hit::Served;
    }
    if (m.index.empty()) return Hit::Miss;
    if (auto* f = find_file(m, m.index)) {
      serve_embedded(m, req, res, *f);
      return Hit::Served;
    }
    return Hit::Miss;
  }
  auto key = join_segs(rel.segs);
  if (!rel.dir) {
    if (auto* f = find_file(m, key)) {
      serve_embedded(m, req, res, *f);
      return Hit::Served;
    }
    if (!m.index.empty() && has_child(m, key)) {
      redirect_to_slash(m, req, res);
      return Hit::Served;
    }
    return Hit::Miss;
  }
  if (m.index.empty()) return Hit::Miss;
  if (auto* f = find_file(m, index_key(key, m.index))) {
    serve_embedded(m, req, res, *f);
    return Hit::Served;
  }
  return Hit::Miss;
}

Hit serve_disk(const Mount& m, const Rel& rel, Request& req, Response& res) {
  int parent = m.root_fd;
  Fd owned;
  std::string walked;
  for (std::size_t i = 0; i < rel.segs.size(); ++i) {
    bool last = i + 1 == rel.segs.size();
    auto opened = open_checked(parent, rel.segs[i], m.root_real);
    if (opened.kind == Opened::Blocked) return Hit::Blocked;
    if (opened.kind == Opened::Busy) {
      fail(m, req, res, {503, "server.busy", "server is at capacity", "too many open files"});
      return Hit::Served;
    }
    if (opened.kind != Opened::Ok) return Hit::Miss;
    struct stat st {};
    if (::fstat(opened.fd.get(), &st) != 0) {
      fail(m, req, res, {500, "static.io", "could not read file", errno_text(errno)});
      return Hit::Served;
    }
    if (!walked.empty()) walked += '/';
    walked += rel.segs[i];
    if (!last || rel.dir) {
      if (!S_ISDIR(st.st_mode)) return Hit::Miss;
      owned = std::move(opened.fd);
      parent = owned.get();
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (m.index.empty()) return Hit::Miss;
      redirect_to_slash(m, req, res);
      return Hit::Served;
    }
    return serve_plain_or_encoded(m, req, res, parent, rel.segs[i], opened.fd.get(), st, walked);
  }

  if (rel.segs.empty() && !req.path.ends_with('/')) {
    redirect_to_slash(m, req, res);
    return Hit::Served;
  }
  if (m.index.empty()) return Hit::Miss;
  auto opened = open_checked(parent, m.index, m.root_real);
  if (opened.kind == Opened::Blocked) return Hit::Blocked;
  if (opened.kind == Opened::Busy) {
    fail(m, req, res, {503, "server.busy", "server is at capacity", "too many open files"});
    return Hit::Served;
  }
  if (opened.kind != Opened::Ok) return Hit::Miss;
  auto mime_path = index_key(walked, m.index);
  return serve_opened(m, req, res, parent, m.index, std::move(opened.fd), mime_path);
}

bool serve_spa(const Mount& m, Request& req, Response& res) {
  if (m.index.empty()) return false;
  if (m.root_fd < 0) {
    if (auto* f = find_file(m, m.index)) {
      serve_embedded(m, req, res, *f);
      return true;
    }
    return false;
  }
  auto opened = open_checked(m.root_fd, m.index, m.root_real);
  if (opened.kind != Opened::Ok) return false;
  return serve_opened(m, req, res, m.root_fd, m.index, std::move(opened.fd), m.index) == Hit::Served;
}

std::string prefix_problem(std::string_view prefix) {
  if (prefix.empty() || prefix.front() != '/') return "must start with '/'";
  if (prefix.find('\0') != std::string_view::npos || prefix.find('\\') != std::string_view::npos ||
      prefix.find('%') != std::string_view::npos)
    return "must not contain '%', '\\' or NUL";
  std::size_t i = 1;
  while (i < prefix.size()) {
    auto slash = prefix.find('/', i);
    auto seg = prefix.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
    if (seg.empty() || seg == "." || seg == "..") return "has an empty, '.' or '..' segment";
    if (slash == std::string_view::npos) break;
    i = slash + 1;
  }
  return {};
}

std::optional<std::string> canonical_asset(std::string_view in, bool dotfiles, std::string& why) {
  while (!in.empty() && in.front() == '/') in.remove_prefix(1);
  if (in.empty() || in.back() == '/') {
    why = "is empty or ends with '/'";
    return std::nullopt;
  }
  if (in.find('\\') != std::string_view::npos || in.find('\0') != std::string_view::npos) {
    why = "contains '\\' or NUL";
    return std::nullopt;
  }
  std::string out;
  std::size_t i = 0;
  while (i < in.size()) {
    auto slash = in.find('/', i);
    auto seg = in.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
    if (seg.empty() || seg == "." || seg == "..") {
      why = "has an empty, '.' or '..' segment";
      return std::nullopt;
    }
    if (seg.front() == '.' && !dotfiles) {
      why = "is a dotfile and dotfiles is off";
      return std::nullopt;
    }
    if (!out.empty()) out += '/';
    out.append(seg);
    if (slash == std::string_view::npos) break;
    i = slash + 1;
  }
  return out;
}

bool valid_index(const std::string& index, bool dotfiles, std::string& why) {
  if (index.empty()) return true;
  if (index.find('/') != std::string::npos || index.find('\\') != std::string::npos ||
      index.find('\0') != std::string::npos || index == "." || index == "..") {
    why = "must be a single file name";
    return false;
  }
  if (index.front() == '.' && !dotfiles) {
    why = "is a dotfile and dotfiles is off";
    return false;
  }
  return true;
}

void build_mount(std::vector<Mount>& mounts, std::string prefix, const files& spec, std::vector<std::string>& errors) {
  auto fail_at = [&](std::string msg) { errors.push_back("static files: " + std::move(msg)); };
  bool disk = !spec.root.empty();
  bool embed = !spec.assets.empty();
  if (disk && embed) return fail_at("set root or assets, not both");
  if (!disk && !embed) return fail_at("set root or assets");

  while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
  if (auto why = prefix_problem(prefix); !why.empty())
    return fail_at("mount \"" + prefix + "\" " + why);

  std::string index_why;
  if (!valid_index(spec.index, spec.dotfiles, index_why)) return fail_at("index \"" + spec.index + "\" " + index_why);
  if (spec.spa && spec.index.empty()) return fail_at("spa needs an index file");

  std::string cache = spec.cache_control.empty() ? std::string("no-cache") : spec.cache_control;
  if (!http::valid_header_value(cache)) return fail_at("cache-control has a character not allowed in a header value");

  Mount m;
  m.prefix = std::move(prefix);
  m.label = m.prefix == "/" ? "/{path}" : m.prefix + "/{path}";
  m.index = spec.index;
  m.dotfiles = spec.dotfiles;
  m.spa = spec.spa;
  m.cache_control = std::move(cache);
  m.precompressed = spec.precompressed;
  m.fallthrough = spec.fallthrough;
  m.max_bytes = spec.max_bytes;
  for (auto& rule : spec.cache) {
    if (rule.glob.empty()) {
      fail_at("cache rule has an empty glob");
      return;
    }
    if (!http::valid_header_value(rule.control)) {
      fail_at("cache rule \"" + rule.glob + "\" has a character not allowed in a header value");
      return;
    }
    m.cache.push_back(rule);
  }
  for (auto& rule : spec.headers) {
    if (rule.glob.empty()) {
      fail_at("header rule has an empty glob");
      return;
    }
    for (auto& [name, value] : rule.fields) {
      if (!http::valid_header_name(name) || !http::valid_header_value(value)) {
        fail_at("header rule \"" + rule.glob + "\" has a name or value not allowed in HTTP headers");
        return;
      }
    }
    m.headers.push_back(rule);
  }

  for (auto& [ext_in, type] : spec.types) {
    std::string ext = ext_in;
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    ext = lower_copy(ext);
    if (ext.empty() || ext.find('.') != std::string::npos || ext.find('/') != std::string::npos) {
      fail_at("type extension \"" + ext_in + "\" is not a single extension");
      return;
    }
    if (type.empty() || !http::valid_header_value(type)) {
      fail_at("content type for \"" + ext + "\" is empty or has a character not allowed in a header value");
      return;
    }
    m.types.emplace_back(std::move(ext), type);
  }

  if (embed) {
    for (auto& a : spec.assets) {
      std::string why;
      auto path = canonical_asset(a.path, spec.dotfiles, why);
      if (!path) {
        fail_at("asset \"" + std::string(a.path) + "\" " + why);
        return;
      }
      if (a.bytes.size() > spec.max_bytes) {
        fail_at("asset \"" + *path + "\" is larger than max_bytes");
        return;
      }
      FileBytes f;
      f.path = std::move(*path);
      f.bytes = a.bytes;
      f.etag = strong_etag(std::string_view(reinterpret_cast<const char*>(a.bytes.data()), a.bytes.size()));
      f.type = mime_of(m, f.path);
      m.files.push_back(std::move(f));
    }
    std::ranges::sort(m.files, {}, &FileBytes::path);
    for (std::size_t i = 1; i < m.files.size(); ++i)
      if (m.files[i].path == m.files[i - 1].path) {
        fail_at("duplicate asset \"" + m.files[i].path + "\"");
        return;
      }
    if (spec.spa && !find_file(m, spec.index)) {
      fail_at("spa index \"" + spec.index + "\" is not one of the assets");
      return;
    }
    mounts.push_back(std::move(m));
    return;
  }

  char resolved[PATH_MAX];
  if (!::realpath(spec.root.c_str(), resolved)) {
    fail_at("root \"" + spec.root + "\" is not a directory (" + errno_text(errno) + ")");
    return;
  }
  int fd = ::open(resolved, kOpen | O_DIRECTORY);
  if (fd < 0) {
    fail_at("root \"" + spec.root + "\" is not a directory (" + errno_text(errno) + ")");
    return;
  }
  m.root_fd = fd;
  m.root_real = resolved;
  if (spec.spa) {
    auto opened = open_checked(fd, spec.index, m.root_real);
    if (opened.kind != Opened::Ok) {
      fail_at("spa index \"" + spec.index + "\" is missing from \"" + spec.root + "\"");
      return;
    }
    struct stat st {};
    if (::fstat(opened.fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
      fail_at("spa index \"" + spec.index + "\" is not a regular file");
      return;
    }
  }
  mounts.push_back(std::move(m));
}

}  // namespace

struct StaticTable {
  std::vector<Mount> mounts;
};

Core::Core() = default;
Core::~Core() = default;

void prepare_static(Core& core, std::vector<std::string>& errors) {
  if (core.static_files.empty()) {
    core.static_table.reset();
    return;
  }
  auto table = std::make_unique<StaticTable>();
  auto before = errors.size();
  for (auto& fm : core.static_files) build_mount(table->mounts, fm.base, fm.spec, errors);
  if (errors.size() != before) {
    core.static_table.reset();
    return;
  }
  core.static_table = std::move(table);
}

std::vector<const Mount*> matching_mounts(const std::vector<Mount>& mounts, std::string_view path) {
  std::vector<const Mount*> out;
  for (auto& m : mounts)
    if (prefix_matches(m, path)) out.push_back(&m);
  std::stable_sort(out.begin(), out.end(), [](const Mount* a, const Mount* b) { return a->prefix.size() > b->prefix.size(); });
  return out;
}

bool static_wants_worker(const Core& core, http::Method method, std::string_view path) {
  if (!core.static_table) return false;
  if (method != http::Method::Get && method != http::Method::Head) return false;
  for (auto* m : matching_mounts(core.static_table->mounts, path)) {
    if (m->root_fd >= 0) return true;
    if (!m->fallthrough) return false;
  }
  return false;
}

bool serve_static(Core& core, Request& req, Response& res) {
  if (!core.static_table) return false;
  if (req.method != http::Method::Get && req.method != http::Method::Head) return false;
  for (auto* m : matching_mounts(core.static_table->mounts, req.path)) {
    std::string rel;
    if (!strip_prefix(*m, req.path, rel)) continue;
    auto parts = split_rel(rel, m->dotfiles);
    // A bad path (dotfile, "..", a slash in a decoded name) is not a missing file.
    if (parts.blocked) return false;
    Hit out = m->root_fd >= 0 ? serve_disk(*m, parts, req, res) : serve_embed(*m, parts, req, res);
    if (out == Hit::Served) return true;
    if (out == Hit::Miss && m->spa && spa_eligible(parts) && serve_spa(*m, req, res)) return true;
    if (!m->fallthrough) return false;
    res = Response{};
  }
  return false;
}

}  // namespace crocket::detail
