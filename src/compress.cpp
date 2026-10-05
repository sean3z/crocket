#include "compress.hpp"

#include "crocket/grpc.hpp"

#include <zlib.h>

#include <charconv>

namespace crocket::detail {
namespace {

constexpr std::size_t kMinBytes = 1024;  // below this, headers and framing outweigh the saving

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

/// Text formats compress well; images, archives and video already are compressed.
bool compressible(std::string_view content_type) {
  auto t = lower(trim(content_type.substr(0, content_type.find(';'))));
  return t.starts_with("text/") || t == "application/json" || t == "application/javascript" ||
         t == "application/xml" || t == "image/svg+xml" || t.ends_with("+json") || t.ends_with("+xml");
}

/// Whether Accept-Encoding allows gzip: named with q > 0, or covered by "*".
bool accepts_gzip(std::string_view header) {
  double gzip = -1, any = -1;
  while (!header.empty()) {
    auto comma = header.find(',');
    auto item = trim(header.substr(0, comma));
    header.remove_prefix(comma == std::string_view::npos ? header.size() : comma + 1);
    auto semi = item.find(';');
    auto coding = lower(trim(item.substr(0, semi)));
    double q = 1;
    if (semi != std::string_view::npos) {
      auto param = trim(item.substr(semi + 1));
      if (param.starts_with("q=") || param.starts_with("Q=")) {
        auto v = param.substr(2);
        if (std::from_chars(v.data(), v.data() + v.size(), q).ec != std::errc()) q = 0;
      }
    }
    if (coding == "gzip" || coding == "x-gzip") gzip = q;
    else if (coding == "*") any = q;
  }
  return gzip >= 0 ? gzip > 0 : any > 0;
}

bool gzip(std::string_view in, std::string& out) {
  z_stream z{};
  if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
  out.resize(deflateBound(&z, uLong(in.size())));
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  z.avail_in = uInt(in.size());
  z.next_out = reinterpret_cast<Bytef*>(out.data());
  z.avail_out = uInt(out.size());
  int rc = deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  return rc == Z_STREAM_END;
}

}  // namespace

void add_vary(Response& rs, std::string_view header) {
  auto current = rs.headers.get("vary");
  if (!current || trim(*current).empty()) {
    rs.headers.set("vary", header);
    return;
  }
  std::string_view rest = *current;
  while (!rest.empty()) {
    auto comma = rest.find(',');
    auto item = trim(rest.substr(0, comma));
    if (item == "*" || Headers::iequals(item, header)) return;
    rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
  }
  rs.headers.set("vary", std::string(*current) + ", " + std::string(header));
}

void compress(const Request& rq, Response& rs) {
  if (rs.status < 200 || rs.status == 204 || rs.status == 206 || rs.status == 304) return;
  if (rs.body.size() < kMinBytes || rs.headers.contains("content-encoding")) return;
  auto ct = rs.headers.get("content-type");
  if (!ct || !compressible(*ct) || grpc::is_grpc_content_type(*ct)) return;
  if (auto cc = rs.headers.get("cache-control"); cc && lower(*cc).contains("no-transform")) return;
  if (rs.headers.contains("accept-ranges")) return;  // byte ranges of the uncompressed body
  // From here the answer depends on Accept-Encoding, compressed or not.
  add_vary(rs, "accept-encoding");
  if (!accepts_gzip(rq.header("accept-encoding").value_or(""))) return;
  std::string out;
  if (!gzip(rs.body, out) || out.size() >= rs.body.size()) return;
  rs.body = std::move(out);
  rs.headers.set("content-encoding", "gzip");
}

}  // namespace crocket::detail
