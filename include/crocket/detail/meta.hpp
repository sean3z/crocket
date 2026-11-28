#pragma once
// Names the two reflection libraries spell differently.
//
// libstdc++ (GCC 16) filters annotations with annotations_of_with_type and
// reports a consteval failure by throwing std::meta::exception, which the
// compiler prints. Bloomberg's clang-p2996 libc++ spells the filter
// annotations_of(info, info) and has no std::meta::exception (P3068 is not
// implemented there). On that compiler the same throw is a plain exception,
// so the program still fails to be a constant expression, without the message
// text in the diagnostic.

#include <meta>
#include <exception>
#include <string_view>

namespace crocket::detail {

consteval auto annotations_of_type(std::meta::info r, std::meta::info type) {
#if defined(__clang__)
  return std::meta::annotations_of(r, type);
#else
  return std::meta::annotations_of_with_type(r, type);
#endif
}

// The vector from the query allocates. On clang-p2996 that allocation is not
// tracked as freed when the temporary is the condition of if constexpr, so the
// condition is not a constant expression. Asking from a consteval function
// destroys the vector before the bool is returned, which both compilers accept.
consteval bool has_annotation(std::meta::info r, std::meta::info type) {
  return !annotations_of_type(r, type).empty();
}

#if defined(__clang__)
struct meta_exception : std::exception {
  consteval meta_exception(std::u8string_view, std::meta::info) noexcept {}
  consteval meta_exception(std::string_view, std::meta::info) noexcept {}
  const char* what() const noexcept override { return "crocket reflection error"; }
};
#endif

[[noreturn]] consteval void reflect_error(std::u8string_view what, std::meta::info from) {
#if defined(__clang__)
  throw meta_exception(what, from);
#else
  throw std::meta::exception(what, from);
#endif
}

}  // namespace crocket::detail
