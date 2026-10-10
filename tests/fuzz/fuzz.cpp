// The fuzz driver.
//
//   crocket_fuzz <target>|all [--seconds N] [--runs N] [--seed N] [--corpus DIR] [--save]
//   crocket_fuzz <target> --replay FILE...
//   crocket_fuzz --list
//
// Every corpus file of the target runs first (so a saved crash stays fixed),
// then mutations of the corpus until the time or run budget is spent. Built
// with -fsanitize-coverage=trace-pc (CROCKET_FUZZ_COVERAGE, ./dev fuzz), an
// input that reaches new code joins the corpus; --save also writes it to the
// corpus directory. A crash, a sanitizer report, a hang or a failed invariant
// leaves the input in crash-<target>-<hash> in the current directory.

#include "fuzz.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>

#ifndef CROCKET_FUZZ_CORPUS
#define CROCKET_FUZZ_CORPUS "tests/fuzz/corpus"
#endif

namespace fuzz {

std::vector<Target>& targets() {
  static std::vector<Target> all;
  return all;
}

namespace {

// ---- coverage -------------------------------------------------------------------------
// GCC calls __sanitizer_cov_trace_pc at every basic block of instrumented code.
// Each (previous block, block) pair counts in a map, as AFL does; an input whose
// counts reach a new bucket (1, 2, 3, 4-7, 8-15, 16-31, 32-127, 128+) for any
// pair found something new.

// The hook below runs inside instrumented code, so it touches only plain
// arrays and builtins: anything it called would be instrumented too, and call
// it again.
constexpr std::size_t kMap = 1 << 16;
std::uint8_t g_hits[kMap];
std::uint8_t g_seen[kMap];  // buckets ever reached, per pair
bool g_tracing = false;      // __atomic_* only
bool g_instrumented = false;
thread_local std::uintptr_t t_prev = 0;

void set_tracing(bool on) { __atomic_store_n(&g_tracing, on, __ATOMIC_RELAXED); }

std::uint8_t bucket(std::uint8_t n) {
  if (n >= 128) return 128;
  if (n >= 32) return 64;
  if (n >= 16) return 32;
  if (n >= 8) return 16;
  if (n >= 4) return 8;
  return n == 3 ? 4 : n;  // 1 -> 1, 2 -> 2
}

bool new_coverage() {
  bool found = false;
  for (std::size_t i = 0; i < kMap; ++i) {
    if (!g_hits[i]) continue;
    auto b = bucket(g_hits[i]);
    if (!(g_seen[i] & b)) {
      g_seen[i] |= b;
      found = true;
    }
  }
  return found;
}

std::size_t pairs_seen() { return std::size_t(std::ranges::count_if(g_seen, [](auto b) { return b != 0; })); }
void clear_hits() { std::memset(g_hits, 0, sizeof g_hits); }

// ---- the input being run, for crash reports ---------------------------------------------

std::string& g_current = *new std::string;  // never destroyed: a leak report at exit still saves it
std::string_view g_target = "?";
std::atomic<std::uint64_t> g_run_id{0};
std::atomic<std::int64_t> g_run_started{0};

std::uint64_t fnv(std::string_view s) {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

/// Writes the current input to <kind>-<target>-<hash>; async-signal-safe.
void save_current(const char* kind) {
  char name[160];
  std::size_t n = 0;
  auto put = [&](std::string_view s) {
    for (char c : s)
      if (n + 1 < sizeof name) name[n++] = c;
  };
  put(kind);
  put("-");
  put(g_target);
  put("-");
  auto h = fnv(g_current);
  for (int i = 15; i >= 0; --i) put(std::string_view(&"0123456789abcdef"[(h >> (i * 4)) & 0xF], 1));
  name[n] = '\0';
  int fd = ::open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    (void)!::write(fd, g_current.data(), g_current.size());
    ::close(fd);
  }
  constexpr std::string_view head = "\ncrocket_fuzz: input saved as ";
  (void)!::write(2, head.data(), head.size());
  (void)!::write(2, name, n);
  (void)!::write(2, "\n", 1);
}

extern "C" void on_crash(int sig) {
  save_current("crash");
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

extern "C" void on_sanitizer_death() { save_current("crash"); }

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---- mutation ----------------------------------------------------------------------------

struct Mutator {
  std::mt19937_64 rng;
  const Target& t;
  const std::vector<std::string>& corpus;

  std::size_t below(std::size_t n) { return n ? std::uniform_int_distribution<std::size_t>(0, n - 1)(rng) : 0; }

  std::string_view token() {
    static const std::vector<std::string> common = {
        "0",      "-1",      "1e309",   "-0",     "4294967296", "18446744073709551616", "9223372036854775808",
        "\\u0000", "\\ud800", "%",       "%zz",    "%00",        "\xff",                 "\xc3\x28",
        "\xf0\x9f\x98\x80", "\r\n", "",  "\"",     "{",          "}",                    "[",
        "]",      ",",       ":",       "/",      "?",          "&",                    "="};
    auto pick = below(common.size() + t.dict.size());
    return pick < t.dict.size() ? std::string_view(t.dict[pick]) : std::string_view(common[pick - t.dict.size()]);
  }

  void once(std::string& s) {
    switch (below(11)) {
      case 0:  // flip a bit
        if (!s.empty()) s[below(s.size())] ^= char(1 << below(8));
        break;
      case 1:  // a random byte
        if (!s.empty()) s[below(s.size())] = char(below(256));
        break;
      case 2: {  // an interesting byte
        static constexpr char bytes[] = {0, 1, 0x7f, char(0x80), char(0xff), '"', '\\', '{', '[', '%', '/', '\n'};
        if (!s.empty()) s[below(s.size())] = bytes[below(sizeof bytes)];
        break;
      }
      case 3: {  // insert random bytes
        std::string ins(1 + below(4), '\0');
        for (auto& c : ins) c = char(below(256));
        s.insert(below(s.size() + 1), ins);
        break;
      }
      case 4:  // delete a range
        if (!s.empty()) {
          auto at = below(s.size());
          s.erase(at, 1 + below(std::min<std::size_t>(16, s.size() - at)));
        }
        break;
      case 5:  // duplicate a range
        if (!s.empty()) {
          auto at = below(s.size());
          auto piece = s.substr(at, 1 + below(std::min<std::size_t>(32, s.size() - at)));
          s.insert(below(s.size() + 1), piece);
        }
        break;
      case 6:  // insert a token
        s.insert(below(s.size() + 1), token());
        break;
      case 7: {  // overwrite with a token
        auto tok = token();
        if (s.size() >= tok.size()) s.replace(below(s.size() - tok.size() + 1), tok.size(), tok);
        break;
      }
      case 8: {  // splice with another corpus entry
        auto& other = corpus[below(corpus.size())];
        s = s.substr(0, below(s.size() + 1)) + other.substr(below(other.size() + 1));
        break;
      }
      case 9: {  // replace a run of digits with a number at some edge
        auto at = s.find_first_of("0123456789", below(s.size() + 1));
        if (at == std::string::npos) break;
        auto end = s.find_first_not_of("0123456789", at);
        static constexpr std::string_view edges[] = {"0", "1", "255", "256", "65535", "65536", "2147483647", "2147483648",
                                                     "4294967295", "9223372036854775807", "99999999999999999999"};
        s.replace(at, (end == std::string::npos ? s.size() : end) - at, edges[below(std::size(edges))]);
        break;
      }
      default:  // truncate
        s.resize(below(s.size() + 1));
    }
  }

  std::string next() {
    std::string s = corpus[below(corpus.size())];
    for (std::size_t i = 0, n = std::size_t(1) << below(4); i < n; ++i) once(s);
    if (s.size() > t.max_len) s.resize(t.max_len);
    return s;
  }
};

// ---- running ----------------------------------------------------------------------------

void run_one(const Target& t, std::string_view input) {
  g_current.assign(input);
  g_run_started.store(now_ns());
  g_run_id.fetch_add(1);
  clear_hits();
  t_prev = 0;
  set_tracing(true);
  t.run(g_current);
  set_tracing(false);
  g_run_started.store(0);
}

std::vector<std::string> load_dir(const std::filesystem::path& dir) {
  std::vector<std::pair<std::string, std::string>> files;  // name, bytes: sorted, so runs repeat
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    std::ifstream in(e.path(), std::ios::binary);
    files.emplace_back(e.path().filename().string(), std::string(std::istreambuf_iterator<char>(in), {}));
  }
  std::ranges::sort(files);
  std::vector<std::string> out;
  for (auto& [name, bytes] : files) out.push_back(std::move(bytes));
  return out;
}

struct Options {
  double seconds = 10;
  std::uint64_t runs = 0;  // 0: until the time is up
  std::uint64_t seed = 0;
  std::filesystem::path corpus = CROCKET_FUZZ_CORPUS;
  bool save = false;
  std::vector<std::string> replay;
};

int fuzz_target(const Target& t, const Options& o) {
  g_target = t.name;
  if (t.setup) t.setup();
  auto dir = o.corpus / std::string(t.name);
  std::vector<std::string> corpus = load_dir(dir);
  if (corpus.empty()) corpus.emplace_back();
  std::size_t seeds = corpus.size();

  for (auto& s : corpus) {
    run_one(t, s);
    new_coverage();
  }
  std::mt19937_64 rng(o.seed ? o.seed : std::random_device{}());
  Mutator m{rng, t, corpus};
  auto start = std::chrono::steady_clock::now();
  auto until = start + std::chrono::duration<double>(o.seconds);
  std::uint64_t runs = 0, added = 0;
  while (o.runs ? runs < o.runs : std::chrono::steady_clock::now() < until) {
    auto input = m.next();
    run_one(t, input);
    ++runs;
    if (new_coverage()) {
      ++added;
      if (o.save) {
        std::filesystem::create_directories(dir);
        char name[17];
        std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(fnv(input)));
        std::ofstream(dir / name, std::ios::binary) << input;
      }
      corpus.push_back(std::move(input));
    }
  }
  if (t.teardown) t.teardown();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (__atomic_load_n(&g_instrumented, __ATOMIC_RELAXED))
    std::printf("  %-10s %9llu runs (%7.0f/s)  %4zu seeds  +%llu inputs reaching new code  %zu edges\n",
                std::string(t.name).c_str(), static_cast<unsigned long long>(runs), secs > 0 ? runs / secs : 0.0, seeds,
                static_cast<unsigned long long>(added), pairs_seen());
  else
    std::printf("  %-10s %9llu runs (%7.0f/s)  %4zu seeds  (no coverage: ./dev fuzz builds with it)\n",
                std::string(t.name).c_str(), static_cast<unsigned long long>(runs), secs > 0 ? runs / secs : 0.0, seeds);
  std::fflush(stdout);
  return 0;
}

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: crocket_fuzz <target>|all [--seconds N] [--runs N] [--seed N] [--corpus DIR] [--save]\n"
               "       crocket_fuzz <target> --replay FILE...\n"
               "       crocket_fuzz --list\n");
  std::exit(2);
}

}  // namespace

void fail(std::string_view what) {
  std::fprintf(stderr, "\ncrocket_fuzz: %.*s: invariant broken: %.*s\n", int(g_target.size()), g_target.data(),
               int(what.size()), what.data());
  save_current("crash");
  std::fflush(stderr);
  std::_Exit(1);
}

}  // namespace fuzz

extern "C" [[gnu::no_sanitize_coverage, gnu::no_sanitize_address]] void __sanitizer_cov_trace_pc() {
  using namespace fuzz;
  __atomic_store_n(&g_instrumented, true, __ATOMIC_RELAXED);
  if (!__atomic_load_n(&g_tracing, __ATOMIC_RELAXED)) return;
  auto pc = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  std::uintptr_t cur = (pc ^ (pc >> 17)) * 0x9E3779B97F4A7C15ull >> 48;
  std::uint8_t* hits = g_hits + ((cur ^ t_prev) & (kMap - 1));
  if (*hits != 255) ++*hits;  // racy between threads (h2): a count is a hint, not a total
  t_prev = cur >> 1;
}

extern "C" void __sanitizer_set_death_callback(void (*)()) __attribute__((weak));

int main(int argc, char** argv) {
  using namespace fuzz;
  if (argc < 2) usage();
  std::string_view which = argv[1];
  if (which == "--list") {
    for (auto& t : targets()) std::printf("%s\n", std::string(t.name).c_str());
    return 0;
  }
  Options o;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    auto value = [&] {
      if (i + 1 >= argc) usage();
      return std::string(argv[++i]);
    };
    if (a == "--seconds") o.seconds = std::stod(value());
    else if (a == "--runs") o.runs = std::stoull(value());
    else if (a == "--seed") o.seed = std::stoull(value());
    else if (a == "--corpus") o.corpus = value();
    else if (a == "--save") o.save = true;
    else if (a == "--replay") {
      while (i + 1 < argc) o.replay.emplace_back(argv[++i]);
    } else usage();
  }

  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) std::signal(sig, on_crash);
  if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(on_sanitizer_death);

  std::vector<const Target*> chosen;
  for (auto& t : targets())
    if (which == "all" || which == t.name) chosen.push_back(&t);
  if (chosen.empty()) {
    std::fprintf(stderr, "crocket_fuzz: no target '%s' (--list shows them)\n", std::string(which).c_str());
    return 2;
  }

  // A watchdog: one input running past its target's timeout is a hang.
  std::atomic<int> timeout_s{3};
  std::thread([&] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      auto started = g_run_started.load();
      if (started && now_ns() - started > std::int64_t(timeout_s.load()) * 1'000'000'000) {
        std::fprintf(stderr, "\ncrocket_fuzz: %.*s: one input ran for more than %d s\n", int(g_target.size()),
                     g_target.data(), timeout_s.load());
        save_current("hang");
        std::_Exit(1);
      }
    }
  }).detach();

  if (!o.replay.empty()) {
    if (chosen.size() != 1) usage();
    auto& t = *chosen[0];
    g_target = t.name;
    timeout_s.store(t.timeout_s);
    if (t.setup) t.setup();
    for (auto& f : o.replay) {
      std::ifstream in(f, std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "crocket_fuzz: cannot read %s\n", f.c_str());
        return 2;
      }
      run_one(t, std::string(std::istreambuf_iterator<char>(in), {}));
      std::printf("  %s: ok\n", f.c_str());
    }
    if (t.teardown) t.teardown();
    return 0;
  }
  for (auto* t : chosen) {
    timeout_s.store(t->timeout_s);
    clear_hits();
    std::memset(g_seen, 0, sizeof g_seen);
    fuzz_target(*t, o);
  }
  return 0;
}
