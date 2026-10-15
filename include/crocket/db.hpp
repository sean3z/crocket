#pragma once
// crocket::db: plain structs in and out of a database. Handlers see only Db&;
// the backend is a CMake target: crocket::pg (Postgres, through libpqxx) today.
//
//   struct Hero { std::int64_t id; std::string name; std::string alias; std::optional<std::string> mentor; };
//
//   [[= http::get("/heroes/{id}")]]
//   std::optional<Hero> get(Db& db, std::int64_t id) { return db.find<Hero>(id); }
//
//   int main() {
//     auto cfg = Config::from_env();                 // CROCKET_DATABASE_URL, CROCKET_DATABASE_POOL_SIZE
//     return build(cfg).manage(db::Pool(cfg.database)).mount("/", reflect_routes<^^api>()).launch();
//   }
//
// No annotations and no base class. Reflection reads the struct:
//   * the table is the type's name in plural snake_case (Hero -> heroes,
//     HeroTeam -> hero_teams); `template <> constexpr auto db::table_name<Hero> = "justice_league";`
//     replaces it;
//   * every data member is a column of the same name, in declaration order;
//   * the member named `id` is the key, filled in by the database on insert.
// find/insert/update/erase SQL is built while compiling. At run time every
// statement is prepared once per connection, and rows are decoded straight
// into the struct's members. std::optional<T> members are NULL when empty.
//
// Every query is timed with steady_clock: crocket_db_seconds{table,op} in
// Metrics, and db_ms, db_wait_ms and db_queries on the request's log line.
// Handlers that never take Db& pay nothing.

#include "crocket/app.hpp"
#include "crocket/extract.hpp"
#include "crocket/pool.hpp"
#include "crocket/request.hpp"

#include <pqxx/pqxx>

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <meta>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace crocket::db {

// ---- names, from reflection ----------------------------------------------------

namespace detail {

/// "HeroTeam" -> "hero_teams", "Category" -> "categories", "Box" -> "boxes".
consteval std::string table_from_type_name(std::string_view n) {
  std::string s;
  for (std::size_t i = 0; i < n.size(); ++i) {
    char c = n[i];
    bool upper = c >= 'A' && c <= 'Z';
    if (upper && i > 0) {
      char p = n[i - 1];
      bool prev_lower = (p >= 'a' && p <= 'z') || (p >= '0' && p <= '9');
      bool next_lower = i + 1 < n.size() && n[i + 1] >= 'a' && n[i + 1] <= 'z';
      bool prev_upper = p >= 'A' && p <= 'Z';
      if (prev_lower || (prev_upper && next_lower)) s += '_';  // "HTTPRoute" -> "http_route"
    }
    s += upper ? char(c - 'A' + 'a') : c;
  }
  auto ends = [&](std::string_view suf) { return s.size() >= suf.size() && s.ends_with(suf); };
  auto vowel = [](char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; };
  if (s.empty()) return s;
  char last = s.back();
  char before = s.size() >= 2 ? s[s.size() - 2] : 'a';
  if (last == 'y' && !vowel(before)) {
    s.pop_back();
    s += "ies";
  } else if (ends("s") || ends("x") || ends("z") || ends("ch") || ends("sh")) {
    s += "es";
  } else if (last == 'o' && !vowel(before)) {
    // hero -> heroes, potato -> potatoes; the usual exceptions take an s.
    constexpr std::string_view takes_s[] = {"photo", "piano", "halo", "memo", "logo", "demo", "solo",
                                            "kilo",  "pro",   "todo", "info", "auto", "zero", "repo"};
    bool plain = false;
    for (auto w : takes_s) {
      if (s == w || (s.size() > w.size() && s.ends_with(w) && s[s.size() - w.size() - 1] == '_')) plain = true;
    }
    s += plain ? "s" : "es";
  } else {
    s += 's';
  }
  return s;
}

consteval std::vector<std::meta::info> fields(std::meta::info t) {
  return std::meta::nonstatic_data_members_of(t, std::meta::access_context::current());
}

consteval std::string number(int n) {
  std::string o;
  do o.insert(o.begin(), char('0' + n % 10));
  while (n /= 10);
  return o;
}

consteval std::string quoted(std::string_view id) {
  std::string o = "\"";
  for (char c : id) {
    if (c == '"') o += '"';
    o += c;
  }
  return o + "\"";
}

}  // namespace detail

/// The table a struct lives in. Specialize to override:
///   template <> constexpr auto crocket::db::table_name<Hero> = "justice_league";
template <class T>
constexpr auto table_name = std::define_static_string(detail::table_from_type_name(std::meta::identifier_of(^^T)));

/// A struct crocket::db can store: an aggregate with a data member named `id`.
template <class T>
concept Model = std::is_aggregate_v<T> && std::is_class_v<T> && [] consteval {
  for (auto m : detail::fields(^^T))
    if (std::meta::identifier_of(m) == "id") return true;
  return false;
}();

namespace detail {

template <class T>
consteval std::meta::info key_member() {
  for (auto m : fields(^^T))
    if (std::meta::identifier_of(m) == "id") return m;
  throw "crocket::db: no member named id";
}

template <class T>
using key_t = [:std::meta::remove_cv(std::meta::type_of(key_member<T>())):];

consteval std::string column_list(std::meta::info t) {
  std::string o;
  for (auto m : fields(t)) {
    if (!o.empty()) o += ", ";
    o += quoted(std::meta::identifier_of(m));
  }
  return o;
}

/// The statements for T, built once while compiling.
template <class T>
struct Statements {
  static consteval std::string table() { return quoted(table_name<T>); }
  static constexpr const char* table_label = table_name<T>;
  static constexpr const char* table_sql = std::define_static_string(table());
  static constexpr const char* select = std::define_static_string("select " + column_list(^^T) + " from " + table());
  static constexpr const char* find = std::define_static_string(
      "select " + column_list(^^T) + " from " + table() + " where \"id\" = $1");
  static constexpr const char* insert = std::define_static_string([] consteval {
    std::string cols, vals;
    int n = 0;
    for (auto m : fields(^^T)) {
      if (std::meta::identifier_of(m) == "id") continue;
      if (n) cols += ", ", vals += ", ";
      cols += quoted(std::meta::identifier_of(m));
      vals += "$" + number(++n);
    }
    return "insert into " + table() + " (" + cols + ") values (" + vals + ") returning \"id\"";
  }());
  static constexpr const char* update = std::define_static_string([] consteval {
    std::string sets;
    int n = 0;
    for (auto m : fields(^^T)) {
      if (std::meta::identifier_of(m) == "id") continue;
      if (n) sets += ", ";
      sets += quoted(std::meta::identifier_of(m)) + " = $" + number(++n);
    }
    return "update " + table() + " set " + sets + " where \"id\" = $" + number(n + 1);
  }());
  static constexpr const char* erase = std::define_static_string("delete from " + table() + " where \"id\" = $1");
};

template <std::meta::info M>
inline constexpr const char* column_sql = std::define_static_string(quoted(std::meta::identifier_of(M)));

/// The quoted column of a member pointer: a comparison per member of the same
/// type against constants, no strings and no lookup.
template <class T, class M>
const char* column_of(M T::* p) {
  const char* out = nullptr;
  template for (constexpr auto m : std::define_static_array(fields(^^T))) {
    if constexpr (std::is_same_v<typename[:std::meta::type_of(m):], M>) {
      if (p == &[:m:]) out = column_sql<m>;
    }
  }
  return out;
}

template <class X>
struct is_optional : std::false_type {};
template <class X>
struct is_optional<std::optional<X>> : std::true_type {};

template <class X>
void read_field(const auto& f, X& out) {  // pqxx::field, or pqxx::field_ref (libpqxx 8)
  if constexpr (is_optional<X>::value) {
    if (f.is_null()) out.reset();
    else out = f.template as<typename X::value_type>();
  } else {
    out = f.template as<X>();
  }
}

/// A row, columns in member order, into a T.
/// Row is pqxx::row, or pqxx::row_ref when iterating a result in libpqxx 8.
template <class T, class Row>
T decode(const Row& r) {
  T out{};
  constexpr auto ms = std::define_static_array(fields(^^T));
  template for (constexpr std::size_t i : std::define_static_array(std::views::iota(std::size_t{0}, ms.size()))) {
    read_field(r[static_cast<int>(i)], out.[:ms[i]:]);
  }
  return out;
}

enum class Op : std::uint8_t { Find, Insert, Update, Erase, Where, Count, Sql };

/// One crocket_db_seconds{table,op} histogram.
struct Series;
Series* series(const char* table, Op op);
void record(Series* s, Clock::duration d) noexcept;

template <class T, Op O>
Series* series_of() {
  static Series* const s = series(Statements<T>::table_label, O);
  return s;
}

/// One pooled connection and the statements prepared on it.
struct Connection {
  std::string url;
  std::unique_ptr<pqxx::connection> conn;
  std::unordered_map<const char*, std::string> prepared;  // static SQL -> statement name
  std::unordered_map<std::string, std::string> dynamic;   // composed SQL (where, sql) -> name
  unsigned next = 0;

  explicit Connection(std::string u);
  pqxx::connection& get();
  const std::string& prepare(const char* sql);
  /// nullptr past the cache's cap: run it unprepared.
  const std::string* prepare(const std::string& sql);
  void reset() noexcept;
};

struct PoolState;

/// Text (or NULL) for a statement parameter, owned by a Query.
using Arg = std::optional<std::string>;

template <class V>
Arg to_arg(const V& v) {
  if constexpr (std::is_same_v<V, std::nullopt_t>) return std::nullopt;
  else if constexpr (is_optional<V>::value) return v ? to_arg(*v) : std::nullopt;
  else if constexpr (std::is_convertible_v<const V&, std::string_view>) return std::string(std::string_view(v));
  else return pqxx::to_string(v);
}

/// A Postgres array literal, {"a","b\"c"}: every element quoted, so it works for
/// text and numbers alike. Built here because libpqxx 8's array conversion needs
/// the connection's encoding, which a query being composed does not have yet.
template <class R>
std::string array_literal(const R& values) {
  std::string out = "{";
  for (const auto& v : values) {
    if (out.size() > 1) out += ',';
    Arg a = to_arg(v);
    if (!a) { out += "NULL"; continue; }
    out += '"';
    for (char ch : *a) {
      if (ch == '"' || ch == '\\') out += '\\';
      out += ch;
    }
    out += '"';
  }
  out += '}';
  return out;
}

}  // namespace detail

// ---- conditions -----------------------------------------------------------------

template <class V>
struct Cond {
  const char* op;  // " = ", " like ", ...
  V value;
};
template <class V>
struct InList {
  std::vector<V> values;
};
template <class V> Cond<std::decay_t<V>> eq(V&& v) { return {" = ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> ne(V&& v) { return {" <> ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> lt(V&& v) { return {" < ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> le(V&& v) { return {" <= ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> gt(V&& v) { return {" > ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> ge(V&& v) { return {" >= ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> like(V&& v) { return {" like ", std::forward<V>(v)}; }
template <class V> Cond<std::decay_t<V>> ilike(V&& v) { return {" ilike ", std::forward<V>(v)}; }
template <class V> InList<V> in(std::initializer_list<V> vs) { return {std::vector<V>(vs)}; }
template <std::ranges::input_range R>
InList<std::ranges::range_value_t<R>> in(const R& vs) { return {{std::ranges::begin(vs), std::ranges::end(vs)}}; }
inline InList<std::string> in(std::initializer_list<const char*> vs) { return {{vs.begin(), vs.end()}}; }

enum class Order : std::uint8_t { Asc, Desc };
inline constexpr Order asc = Order::Asc;
inline constexpr Order desc = Order::Desc;

class Db;

/// `db.where(...)`: conditions joined with AND, then order_by and limit. Runs
/// when its rows are asked for: all(), first(), count(), a range-for, or
/// conversion to std::vector<T>.
template <class T>
class Query {
 public:
  template <class M, class V>
  Query& where(M T::* member, V&& v) & {
    add(detail::column_of(member), std::forward<V>(v));
    return *this;
  }
  template <class M, class V>
  Query&& where(M T::* member, V&& v) && { return std::move(where(member, std::forward<V>(v))); }

  template <class M>
  Query& order_by(M T::* member, Order o = asc) & {
    if (!order_.empty()) order_ += ", ";
    order_ += detail::column_of(member);
    if (o == desc) order_ += " desc";
    return *this;
  }
  template <class M>
  Query&& order_by(M T::* member, Order o = asc) && { return std::move(order_by(member, o)); }
  Query& limit(std::int64_t n) & { limit_ = n; return *this; }
  Query&& limit(std::int64_t n) && { return std::move(limit(n)); }
  Query& offset(std::int64_t n) & { offset_ = n; return *this; }
  Query&& offset(std::int64_t n) && { return std::move(offset(n)); }

  std::vector<T> all();
  std::optional<T> first();
  std::int64_t count();

  operator std::vector<T>() { return all(); }
  auto begin() { run(); return rows_->begin(); }
  auto end() { run(); return rows_->end(); }

 private:
  friend class Db;
  explicit Query(Db& db) : db_(&db) {}

  template <class V>
  void add(const char* col, V&& v) {
    using D = std::remove_cvref_t<V>;
    sql_ += sql_.empty() ? " where " : " and ";
    sql_ += col;
    if constexpr (std::is_same_v<D, std::nullopt_t>) {
      sql_ += " is null";
    } else if constexpr (requires { v.op; v.value; }) {
      sql_ += v.op;
      push(v.value);
    } else if constexpr (requires { v.values; }) {
      sql_ += " = any(";
      args_.push_back(detail::array_literal(v.values));
      sql_ += "$" + std::to_string(args_.size()) + ")";
    } else {
      sql_ += " = ";
      push(v);
    }
  }
  template <class V>
  void push(const V& v) {
    args_.push_back(detail::to_arg(v));
    sql_ += "$" + std::to_string(args_.size());
  }
  void run() {
    if (!rows_) rows_ = all();
  }
  std::string tail() const;

  Db* db_;
  std::string sql_;    // " where ... and ..."
  std::string order_;  // "\"id\" desc"
  std::optional<std::int64_t> limit_, offset_;
  std::vector<detail::Arg> args_;
  std::optional<std::vector<T>> rows_;
};

/// The database, for one request. Handlers take it as `Db& db`; its pool
/// connection is checked out at the first query and returned when the handler
/// ends. Not for sharing between threads.
class Db {
 public:
  Db(std::shared_ptr<detail::PoolState> pool, Request* rq) : pool_(std::move(pool)), rq_(rq) {}
  Db(Db&&) noexcept = default;
  Db& operator=(Db&&) noexcept = default;
  ~Db();

  /// The row with this id, or nullopt.
  template <Model T>
  std::optional<T> find(const detail::key_t<T>& id) {
    using S = detail::Statements<T>;
    return timed<T, detail::Op::Find>([&](pqxx::connection& c, detail::Connection& pc) -> std::optional<T> {
      pqxx::nontransaction tx{c};
      auto r = tx.exec(pqxx::prepped{pc.prepare(S::find)}, pqxx::params{id});
      if (r.empty()) return std::nullopt;
      return detail::decode<T>(r[0]);
    });
  }

  /// Inserts every member but id, sets id to the one the database chose, and
  /// returns the stored row. A unique constraint violation is 409 db.conflict.
  template <class U, class T = std::remove_cvref_t<U>>
    requires Model<T>
  T insert(U&& value) {
    using S = detail::Statements<T>;
    auto id = timed<T, detail::Op::Insert>([&](pqxx::connection& c, detail::Connection& pc) {
      pqxx::nontransaction tx{c};
      auto r = tx.exec(pqxx::prepped{pc.prepare(S::insert)}, params_without_id(value));
      return r[0][0].template as<detail::key_t<T>>();
    });
    T out = std::forward<U>(value);
    out.id = id;
    if constexpr (std::is_lvalue_reference_v<U> && !std::is_const_v<std::remove_reference_t<U>>) value.id = id;
    return out;
  }

  /// Writes every member to the row with value.id. False when no row has it.
  template <Model T>
  bool update(const T& value) {
    using S = detail::Statements<T>;
    return timed<T, detail::Op::Update>([&](pqxx::connection& c, detail::Connection& pc) {
      pqxx::nontransaction tx{c};
      auto p = params_without_id(value);
      p.append(value.id);
      return tx.exec(pqxx::prepped{pc.prepare(S::update)}, p).affected_rows() > 0;
    });
  }

  /// Deletes the row with this id. False when there was none.
  template <Model T>
  bool erase(const detail::key_t<T>& id) {
    using S = detail::Statements<T>;
    return timed<T, detail::Op::Erase>([&](pqxx::connection& c, detail::Connection& pc) {
      pqxx::nontransaction tx{c};
      return tx.exec(pqxx::prepped{pc.prepare(S::erase)}, pqxx::params{id}).affected_rows() > 0;
    });
  }

  /// Rows whose member equals v, or matches db::like(...), db::gt(...),
  /// db::in({...}) and the rest; std::nullopt is IS NULL.
  template <class T, class M, class V>
    requires Model<T>
  Query<T> where(M T::* member, V&& v) {
    Query<T> q(*this);
    q.where(member, std::forward<V>(v));
    return q;
  }
  /// Every row of T's table (add order_by and limit as needed).
  template <Model T>
  Query<T> all() { return Query<T>(*this); }

  /// The escape hatch: any SQL, $1... bound to args. With a T, the result's
  /// columns are decoded into T's members by position.
  template <class T, class... A>
  std::vector<T> sql(const std::string& query, const A&... args) {
    return timed_raw(detail::Op::Sql, [&](pqxx::connection& c, detail::Connection& pc) {
      auto r = exec_dynamic(c, pc, query, {detail::to_arg(args)...});
      std::vector<T> out;
      out.reserve(r.size());
      for (auto row : r) out.push_back(detail::decode<T>(row));
      return out;
    });
  }
  template <class... A>
  pqxx::result exec(const std::string& query, const A&... args) {
    return timed_raw(detail::Op::Sql, [&](pqxx::connection& c, detail::Connection& pc) {
      return exec_dynamic(c, pc, query, {detail::to_arg(args)...});
    });
  }

  /// The libpqxx connection itself, for what the layer does not cover
  /// (transactions, COPY, LISTEN). Not timed.
  pqxx::connection& connection() { return lease().get(); }

 private:
  template <class T>
  friend class Query;

  template <class T>
  static pqxx::params params_without_id(const T& v) {
    pqxx::params p;
    template for (constexpr auto m : std::define_static_array(detail::fields(^^T))) {
      if constexpr (std::meta::identifier_of(m) != "id") p.append(v.[:m:]);
    }
    return p;
  }

  detail::Connection& lease();
  static pqxx::result exec_dynamic(pqxx::connection& c, detail::Connection& pc, const std::string& sql,
                                   std::vector<detail::Arg> args);
  [[noreturn]] void rethrow(Clock::time_point t0, detail::Series* s, detail::Connection& pc);
  void finish(Clock::time_point t0, detail::Series* s) noexcept;

  template <class T, detail::Op O, class F>
  auto timed(F&& f) {
    return run(detail::series_of<T, O>(), std::forward<F>(f));
  }
  template <class F>
  auto timed_raw(detail::Op op, F&& f) {
    return run(sql_series(op), std::forward<F>(f));
  }
  static detail::Series* sql_series(detail::Op op);

  template <class F>
  auto run(detail::Series* s, F&& f) {
    auto& pc = lease();
    auto t0 = Clock::now();
    try {
      auto out = f(pc.get(), pc);
      finish(t0, s);
      return out;
    } catch (...) {
      rethrow(t0, s, pc);
    }
  }

  std::shared_ptr<detail::PoolState> pool_;
  Request* rq_;
  std::optional<crocket::Pool<detail::Connection>::Lease> lease_;
};

template <class T>
std::string Query<T>::tail() const {
  std::string o = sql_;
  if (!order_.empty()) o += " order by " + order_;
  if (limit_) o += " limit " + std::to_string(*limit_);
  if (offset_) o += " offset " + std::to_string(*offset_);
  return o;
}

template <class T>
std::vector<T> Query<T>::all() {
  std::string q = std::string(detail::Statements<T>::select) + tail();
  return db_->template timed<T, detail::Op::Where>([&](pqxx::connection& c, detail::Connection& pc) {
    auto r = Db::exec_dynamic(c, pc, q, args_);
    std::vector<T> out;
    out.reserve(r.size());
    for (auto row : r) out.push_back(detail::decode<T>(row));
    return out;
  });
}

template <class T>
std::optional<T> Query<T>::first() {
  auto saved = limit_;
  limit_ = 1;
  auto rows = all();
  limit_ = saved;
  if (rows.empty()) return std::nullopt;
  return std::move(rows.front());
}

template <class T>
std::int64_t Query<T>::count() {
  std::string q = std::string("select count(*) from ") + detail::Statements<T>::table_sql + sql_;
  return db_->template timed<T, detail::Op::Count>([&](pqxx::connection& c, detail::Connection& pc) {
    return Db::exec_dynamic(c, pc, q, args_)[0][0].template as<std::int64_t>();
  });
}

/// The connection pool, managed once: `.manage(db::Pool(cfg.database))`.
/// Opens pool_size connections at startup (and throws if it cannot); GET
/// /readyz checks one with `select 1`.
class Pool {
 public:
  static constexpr bool crocket_may_block = true;  // handlers taking Db& stay on workers
  explicit Pool(const DatabaseConfig& cfg);
  Pool(Pool&&) noexcept = default;
  [[nodiscard]] bool ready() const;
  /// A Db outside a request: seeding, migrations, background jobs, tests.
  [[nodiscard]] Db handle() const { return Db(state_, nullptr); }
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t available() const;

 private:
  std::shared_ptr<detail::PoolState> state_;
  friend struct crocket::FromRequest<Db>;
};

}  // namespace crocket::db

namespace crocket {

/// A model's `id` may be left out of a JSON body: the database assigns it, and
/// insert() never sends it. It reads as 0.
template <db::Model T>
struct json::omittable<T> {
  static constexpr bool member(std::string_view name) { return name == "id"; }
};

using db::Db;

/// `Db& db` in a handler: the request's database handle (needs a managed db::Pool).
template <>
struct FromRequest<db::Db> {
  static constexpr std::string_view kind = "db";
  static constexpr bool consumes_body = false;
  static constexpr bool may_block = true;
  static std::expected<db::Db, ApiError> extract(Request& r) {
    auto* p = r.state_registry ? r.state_registry->template get<db::Pool>() : nullptr;
    if (!p) return std::unexpected(ApiError::internal("Db requested but no db::Pool is managed"));
    return db::Db(p->state_, &r);
  }
  static void state_deps(std::vector<detail::StateDep>& out) { out.push_back(detail::state_dep<db::Pool>()); }
};

}  // namespace crocket
