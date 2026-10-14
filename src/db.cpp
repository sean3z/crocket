// crocket::db: the pool, prepared statements per connection, and the
// crocket_db_seconds histograms.

#include "crocket/db.hpp"
#include "crocket/fairings.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

namespace crocket::db {
namespace detail {

namespace {

// Upper bounds in nanoseconds: 100us .. 2.5s.
constexpr std::array<std::int64_t, 13> kBucketsNs = {100'000,     250'000,     500'000,     1'000'000,
                                                     2'500'000,   5'000'000,   10'000'000,  25'000'000,
                                                     50'000'000,  100'000'000, 250'000'000, 1'000'000'000,
                                                     2'500'000'000};
constexpr const char* kOpNames[] = {"find", "insert", "update", "erase", "where", "count", "sql"};

}  // namespace

struct Series {
  const char* table;
  Op op;
  std::array<std::atomic<std::uint64_t>, kBucketsNs.size() + 1> buckets{};  // last: +Inf only
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> sum_ns{0};
};

struct PoolState {
  crocket::Pool<Connection> pool;
  std::size_t size;
  PoolState(std::size_t n, const std::function<Connection()>& make, crocket::Pool<Connection>::Options o)
      : pool(n, make, std::move(o)), size(n) {}
};

namespace {

struct Registry {
  std::mutex mu;
  std::deque<Series> series;                   // stable addresses
  std::vector<std::weak_ptr<PoolState>> pools;
};
Registry& registry() {
  static Registry* r = new Registry;  // never destroyed: statics may record during exit
  return *r;
}

std::string label(std::string_view v) {
  std::string o;
  for (char c : v) {
    if (c == '\\' || c == '"') o += '\\';
    if (c == '\n') { o += "\\n"; continue; }
    o += c;
  }
  return o;
}

void render(std::string& o) {
  auto& r = registry();
  std::lock_guard lk(r.mu);
  if (!r.series.empty()) {
    o += "# HELP crocket_db_seconds Database queries through crocket::db, decode included.\n"
         "# TYPE crocket_db_seconds histogram\n";
    for (auto& s : r.series) {
      auto labels = "table=\"" + label(s.table) + "\",op=\"" + kOpNames[int(s.op)] + "\"";
      std::uint64_t cum = 0;
      for (std::size_t i = 0; i < kBucketsNs.size(); ++i) {
        cum += s.buckets[i].load(std::memory_order_relaxed);
        char le[32];
        std::snprintf(le, sizeof le, "%g", double(kBucketsNs[i]) / 1e9);
        o += "crocket_db_seconds_bucket{" + labels + ",le=\"" + le + "\"} " + std::to_string(cum) + "\n";
      }
      auto n = s.count.load(std::memory_order_relaxed);
      o += "crocket_db_seconds_bucket{" + labels + ",le=\"+Inf\"} " + std::to_string(n) + "\n";
      char sum[48];
      std::snprintf(sum, sizeof sum, "%.6f", double(s.sum_ns.load(std::memory_order_relaxed)) / 1e9);
      o += "crocket_db_seconds_sum{" + labels + "} " + sum + "\n";
      o += "crocket_db_seconds_count{" + labels + "} " + std::to_string(n) + "\n";
    }
  }
  std::size_t total = 0, idle = 0;
  bool any = false;
  for (auto& w : r.pools)
    if (auto p = w.lock()) {
      any = true;
      total += p->size;
      idle += p->pool.available();
    }
  if (any) {
    o += "# HELP crocket_db_pool_connections Database pool connections by state.\n"
         "# TYPE crocket_db_pool_connections gauge\n";
    o += "crocket_db_pool_connections{state=\"idle\"} " + std::to_string(idle) + "\n";
    o += "crocket_db_pool_connections{state=\"in_use\"} " + std::to_string(total - idle) + "\n";
  }
}

}  // namespace

Series* series(const char* table, Op op) {
  crocket::detail::add_metrics_renderer(&render);
  auto& r = registry();
  std::lock_guard lk(r.mu);
  for (auto& s : r.series)
    if (s.op == op && std::string_view(s.table) == table) return &s;
  auto& s = r.series.emplace_back();
  s.table = table;
  s.op = op;
  return &s;
}

void record(Series* s, Clock::duration d) noexcept {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
  std::size_t i = 0;
  while (i < kBucketsNs.size() && ns > kBucketsNs[i]) ++i;
  s->buckets[i].fetch_add(1, std::memory_order_relaxed);
  s->count.fetch_add(1, std::memory_order_relaxed);
  s->sum_ns.fetch_add(std::uint64_t(ns < 0 ? 0 : ns), std::memory_order_relaxed);
}

// ---- connections ------------------------------------------------------------------

constexpr std::size_t kMaxDynamic = 256;  // composed statements prepared per connection

Connection::Connection(std::string u) : url(std::move(u)) { get(); }

pqxx::connection& Connection::get() {
  if (!conn || !conn->is_open()) {
    prepared.clear();
    dynamic.clear();
    conn = std::make_unique<pqxx::connection>(url);
  }
  return *conn;
}

void Connection::reset() noexcept {
  conn.reset();
  prepared.clear();
  dynamic.clear();
}

const std::string& Connection::prepare(const char* sql) {
  if (auto it = prepared.find(sql); it != prepared.end()) return it->second;
  std::string name = "crocket_s" + std::to_string(next++);
  get().prepare(name, sql);
  return prepared.emplace(sql, std::move(name)).first->second;
}

const std::string* Connection::prepare(const std::string& sql) {
  if (auto it = dynamic.find(sql); it != dynamic.end()) return &it->second;
  if (dynamic.size() >= kMaxDynamic) return nullptr;
  std::string name = "crocket_d" + std::to_string(next++);
  get().prepare(name, sql);
  return &dynamic.emplace(sql, std::move(name)).first->second;
}

}  // namespace detail

// ---- Db ------------------------------------------------------------------------------

Db::~Db() = default;

detail::Connection& Db::lease() {
  if (lease_) return **lease_;
  auto t0 = Clock::now();
  Deadline d = rq_ ? rq_->deadline : Deadline{Clock::now() + std::chrono::seconds(30), {}};
  auto l = pool_->pool.checkout(d);
  if (rq_) rq_->db_wait += Clock::now() - t0;
  if (!l) throw l.error();
  lease_.emplace(std::move(*l));
  return **lease_;
}

void Db::finish(Clock::time_point t0, detail::Series* s) noexcept {
  auto d = Clock::now() - t0;
  detail::record(s, d);
  if (rq_) {
    rq_->db_time += d;
    ++rq_->db_queries;
  }
}

void Db::rethrow(Clock::time_point t0, detail::Series* s, detail::Connection& pc) {
  finish(t0, s);
  try {
    throw;
  } catch (const pqxx::unique_violation& e) {
    throw ApiError{409, "db.conflict", "already exists", e.what()};
  } catch (const pqxx::broken_connection&) {
    pc.reset();  // reconnect on the next checkout
    throw;
  } catch (const pqxx::in_doubt_error&) {
    pc.reset();
    throw;
  }
}

detail::Series* Db::sql_series(detail::Op op) {
  static detail::Series* const s = detail::series("<sql>", detail::Op::Sql);
  (void)op;
  return s;
}

pqxx::result Db::exec_dynamic(pqxx::connection& c, detail::Connection& pc, const std::string& sql,
                              std::vector<detail::Arg> args) {
  pqxx::params p;
  for (auto& a : args) {
    if (a) p.append(std::string_view(*a));
    else p.append(std::optional<std::string_view>{});
  }
  pqxx::nontransaction tx{c};
  if (auto* name = pc.prepare(sql)) return tx.exec(pqxx::prepped{*name}, p);
  return tx.exec(pqxx::zview(sql), p);
}

// ---- Pool ------------------------------------------------------------------------------

Pool::Pool(const DatabaseConfig& cfg) {
  std::string url = cfg.url;
  crocket::Pool<detail::Connection>::Options o;
  o.max_wait = cfg.pool_wait;
  o.health = [](detail::Connection& c) {
    try {
      pqxx::nontransaction tx{c.get()};
      tx.exec("select 1");
      return true;
    } catch (...) {
      c.reset();
      return false;
    }
  };
  state_ = std::make_shared<detail::PoolState>(
      cfg.pool_size ? cfg.pool_size : 1, [url] { return detail::Connection(url); }, std::move(o));
  auto& r = detail::registry();
  std::lock_guard lk(r.mu);
  std::erase_if(r.pools, [](auto& w) { return w.expired(); });
  r.pools.push_back(state_);
  crocket::detail::add_metrics_renderer(&detail::render);
}

bool Pool::ready() const { return state_->pool.ready(); }
std::size_t Pool::size() const { return state_->size; }
std::size_t Pool::available() const { return state_->pool.available(); }

}  // namespace crocket::db
