#pragma once
// Managed state: Crocket::manage(T) stores exactly one T; State<T> hands out a
// reference to it. Requirements are declared by extractors at compile time
// and checked at ignite, never on the first request.

#include <meta>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace crocket {

namespace detail {

/// Unique per-type key without RTTI.
template <class T>
inline constexpr char type_tag = 0;
using TypeKey = const void*;
template <class T>
constexpr TypeKey type_key() { return &type_tag<std::remove_cvref_t<T>>; }

template <class T>
consteval const char* type_display_name() {
  return std::define_static_string(std::meta::display_string_of(^^T));
}

/// A dependency on managed state, recorded per route.
struct StateDep {
  TypeKey key;
  std::string_view name;  // "Db"
};

template <class T>
constexpr StateDep state_dep() { return {type_key<T>(), type_display_name<std::remove_cvref_t<T>>()}; }

class StateRegistry {
 public:
  StateRegistry() = default;
  StateRegistry(const StateRegistry&) = delete;
  StateRegistry& operator=(const StateRegistry&) = delete;
  ~StateRegistry() { clear(); }

  /// Returns false if a T is already managed.
  template <class T>
  bool put(T value) {
    if (find(type_key<T>())) return false;
    auto* p = new T(std::move(value));
    entries_.push_back({type_key<T>(), type_display_name<T>(), p, [](void* q) { delete static_cast<T*>(q); }});
    return true;
  }

  [[nodiscard]] void* find(TypeKey key) const {
    for (auto& e : entries_)
      if (e.key == key) return e.ptr;
    return nullptr;
  }
  template <class T>
  [[nodiscard]] T* get() const { return static_cast<T*>(find(type_key<T>())); }

  /// Drops state in reverse manage() order (pools last-in first-out).
  void clear() {
    while (!entries_.empty()) {
      auto e = entries_.back();
      entries_.pop_back();
      e.del(e.ptr);
    }
  }

 private:
  struct Entry {
    TypeKey key;
    std::string_view name;
    void* ptr;
    void (*del)(void*);
  };
  std::vector<Entry> entries_;
};

}  // namespace detail

/// Extractor for managed state. Cheap to copy; refers to the single T owned by
/// the Crocket instance. T must be safe for concurrent use: handlers run on worker threads.
template <class T>
class State {
 public:
  explicit State(T& ref) : ptr_(&ref) {}
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  T& get() const { return *ptr_; }

 private:
  T* ptr_;
};

}  // namespace crocket
