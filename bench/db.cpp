// crocket::db against the same queries written by hand with libpqxx: prepared
// statements, a nontransaction per query, as<>() per column. Both talk to the
// same Postgres over their own connection; rounds alternate so drift hits both.
//
//   CROCKET_BENCH_DATABASE_URL=postgresql://localhost/crocket_bench ./build/crocket_bench_db [iterations] [rounds]

#include <crocket/db.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace crocket;

struct Hero {
  std::int64_t id;
  std::string name;
  std::string alias;
  std::optional<std::string> mentor;
};

static volatile std::int64_t g_sink = 0;

template <class F>
static double per_op_us(int n, F&& f) {
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) f(i);
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
}

static double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

int main(int argc, char** argv) {
  const char* url = std::getenv("CROCKET_BENCH_DATABASE_URL");
  if (!url) {
    std::fprintf(stderr, "set CROCKET_BENCH_DATABASE_URL\n");
    return 2;
  }
  int n = argc > 1 ? std::atoi(argv[1]) : 5000;
  int rounds = argc > 2 ? std::atoi(argv[2]) : 7;

  db::Pool pool({.url = url, .pool_size = 1});
  auto d = pool.handle();
  d.exec("drop table if exists heroes");
  d.exec("create table heroes (id bigserial primary key, name text not null, alias text not null, mentor text)");
  for (int i = 0; i < 1000; ++i) {
    bool robin = i % 4 == 0;
    d.insert(Hero{.name = "Hero " + std::to_string(i), .alias = robin ? "Robin" : "The Flash",
                  .mentor = robin ? std::optional<std::string>("Bruce Wayne") : std::nullopt});
  }
  d.exec("analyze heroes");

  pqxx::connection raw{url};
  raw.prepare("rf", "select id, name, alias, mentor from heroes where id = $1");
  raw.prepare("ri", "insert into heroes (name, alias, mentor) values ($1, $2, $3) returning id");
  raw.prepare("rw", "select id, name, alias, mentor from heroes where mentor = $1 order by id limit 10");
  auto decode = [](const pqxx::row& r) {
    Hero h;
    h.id = r[0].as<std::int64_t>();
    h.name = r[1].as<std::string>();
    h.alias = r[2].as<std::string>();
    if (!r[3].is_null()) h.mentor = r[3].as<std::string>();
    return h;
  };

  std::mt19937_64 rng(42);
  std::vector<std::int64_t> ids(n);
  for (auto& id : ids) id = std::int64_t(rng() % 1000) + 1;

  struct Case {
    const char* name;
    std::vector<double> layer, raw;
  };
  Case cases[] = {{"find", {}, {}}, {"insert", {}, {}}, {"where (10 rows)", {}, {}}};

  for (int r = 0; r < rounds + 1; ++r) {  // round 0 warms up
    double v[6];
    std::function<double()> f[6];
    f[0] = [&] { return per_op_us(n, [&](int i) { g_sink += d.find<Hero>(ids[i])->id; }); };
    f[1] = [&] { return per_op_us(n, [&](int i) {
      pqxx::nontransaction tx{raw};
      auto res = tx.exec(pqxx::prepped{"rf"}, pqxx::params{ids[i]});
      g_sink += decode(res[0]).id;
    }); };
    f[2] = [&] { return per_op_us(n, [&](int) {
      g_sink += d.insert(Hero{.name = "Bart Allen", .alias = "Impulse", .mentor = "Barry Allen"}).id;
    }); };
    f[3] = [&] { return per_op_us(n, [&](int) {
      Hero h{.name = "Bart Allen", .alias = "Impulse", .mentor = "Barry Allen"};
      pqxx::nontransaction tx{raw};
      auto res = tx.exec(pqxx::prepped{"ri"}, pqxx::params{h.name, h.alias, h.mentor});
      h.id = res[0][0].as<std::int64_t>();
      g_sink += h.id;
    }); };
    f[4] = [&] { return per_op_us(n, [&](int) {
      auto rows = d.where(&Hero::mentor, "Bruce Wayne").order_by(&Hero::id).limit(10).all();
      g_sink += rows.size();
    }); };
    f[5] = [&] { return per_op_us(n, [&](int) {
      pqxx::nontransaction tx{raw};
      auto res = tx.exec(pqxx::prepped{"rw"}, pqxx::params{"Bruce Wayne"});
      std::vector<Hero> rows;
      rows.reserve(res.size());
      for (auto row : res) rows.push_back(decode(row));
      g_sink += rows.size();
    }); };
    for (int c = 0; c < 3; ++c) {  // alternate who goes first, so neither always meets a warmer cache
      int a = 2 * c + (r % 2), b = 2 * c + 1 - (r % 2);
      v[a] = f[a]();
      v[b] = f[b]();
    }
    if (r == 0) continue;
    for (int c = 0; c < 3; ++c) {
      cases[c].layer.push_back(v[2 * c]);
      cases[c].raw.push_back(v[2 * c + 1]);
    }
  }

  std::printf("%-18s %12s %12s %9s\n", "op", "crocket::db", "libpqxx", "diff");
  for (auto& c : cases) {
    double l = median(c.layer), r = median(c.raw);
    std::printf("%-18s %10.2fus %10.2fus %+8.2f%%\n", c.name, l, r, (l - r) / r * 100);
  }

  // What the observability costs per query, without the database.
  auto* s = db::detail::series("bench", db::detail::Op::Find);
  constexpr int m = 10'000'000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < m; ++i) {
    auto a = Clock::now();
    db::detail::record(s, Clock::now() - a);
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / m;
  t0 = std::chrono::steady_clock::now();
  std::int64_t acc = 0;
  for (int i = 0; i < m; ++i) {
    auto a = Clock::now();
    acc += (Clock::now() - a).count();
  }
  g_sink += acc;
  double clock_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / m;
  std::printf("timing + histogram per query: %.1f ns (of which two steady_clock reads: %.1f ns)\n", ns, clock_ns);
  return 0;
}
