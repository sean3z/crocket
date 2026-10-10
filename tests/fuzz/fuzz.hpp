#pragma once
// crocket's fuzzer: a target is a function that takes arbitrary bytes and must
// neither crash, hang, nor break its own invariants. The driver (fuzz.cpp)
// feeds it mutations of a seed corpus; in a build with coverage (./dev fuzz),
// an input that reaches new code joins the corpus and is mutated further.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fuzz {

struct Target {
  std::string_view name;
  std::size_t max_len = 4096;          // mutations stay within this many bytes
  std::vector<std::string> dict = {};  // tokens worth inserting (keywords, delimiters)
  void (*run)(std::string_view input) = nullptr;
  void (*setup)() = nullptr;     // once, before the first input
  void (*teardown)() = nullptr;  // once, after the last
  int timeout_s = 3;             // one input running longer is a hang
};

/// An invariant broke: reports `what`, saves the input, and aborts.
[[noreturn]] void fail(std::string_view what);

std::vector<Target>& targets();

struct Register {
  explicit Register(Target t) { targets().push_back(std::move(t)); }
};

}  // namespace fuzz
