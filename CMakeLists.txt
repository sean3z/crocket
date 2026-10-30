cmake_minimum_required(VERSION 3.28)
project(crocket VERSION 0.4.0 LANGUAGES C CXX)

# Examples and tests are for working on crocket; a project that pulls crocket
# in with FetchContent or add_subdirectory doesn't build them.
option(CROCKET_BUILD_EXAMPLES "Build crocket_hello, crocket_serve, crocket_grpc, and crocket_auth_example when auth is on" ${PROJECT_IS_TOP_LEVEL})
option(CROCKET_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
# ./dev fuzz: crocket and the fuzz targets report coverage to tests/fuzz, so
# inputs that reach new code are kept and mutated further.
option(CROCKET_FUZZ_COVERAGE "Instrument crocket for coverage-guided fuzzing" OFF)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# crocket has no C++ modules; skip the dependency scan on every source.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ---- linker ------------------------------------------------------------------
# Prefer lld, then mold, but only if it really links with this compiler: mold
# 2.40 cannot parse GCC 16's libgcc_s_asneeded.so script. Otherwise the default
# linker. Pass -DCMAKE_LINKER_TYPE=... to choose yourself. Skipped when crocket
# is a dependency: then the linker is the consumer's choice.
if(PROJECT_IS_TOP_LEVEL AND NOT DEFINED CMAKE_LINKER_TYPE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  include(CheckLinkerFlag)
  foreach(ld IN ITEMS lld mold)
    check_linker_flag(CXX "-fuse-ld=${ld}" CROCKET_LINKER_${ld}_WORKS)
    if(CROCKET_LINKER_${ld}_WORKS)
      string(TOUPPER ${ld} CMAKE_LINKER_TYPE)
      break()
    endif()
  endforeach()
endif()
if(CMAKE_LINKER_TYPE)
  message(STATUS "crocket: linking with ${CMAKE_LINKER_TYPE}")
endif()

# ---- reflection check ---------------------------------------------------------
# crocket needs C++26 static reflection (P2996). Today that means GCC 16+ with
# -freflection. Fail here with a clear message rather than deep in a header.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++26")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(APPEND CMAKE_REQUIRED_FLAGS " -freflection")
endif()
check_cxx_source_compiles("
#include <meta>
int main() { constexpr auto r = ^^int; return std::meta::is_type(r) ? 0 : 1; }
" CROCKET_HAS_REFLECTION)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT CROCKET_HAS_REFLECTION)
  message(FATAL_ERROR
    "crocket requires a C++26 compiler with static reflection (P2996): "
    "GCC 16+ with -freflection. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
    "(${CMAKE_CXX_COMPILER}) cannot compile a ^^int / std::meta test. "
    "Set CMAKE_CXX_COMPILER to g++-16 or newer.")
endif()

# ---- h2o (the only wire engine) ---------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/h2o.cmake)

# ---- crocket ---------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(crocket STATIC
  src/app.cpp
  src/cpus.cpp
  src/async.cpp
  src/log.cpp
  src/conditional.cpp
  src/identity.cpp
  src/router.cpp
  src/json.cpp
  src/http.cpp
  src/fairings.cpp
  src/openapi.cpp
  src/grpc.cpp
  src/engine_h2o.cpp)
add_library(crocket::crocket ALIAS crocket)

# ---- version ------------------------------------------------------------------
# <crocket/version.hpp>: the project() version, the git commit and the compiler,
# fixed at configure time. CROCKET_GIT_COMMIT can be passed in (Docker builds
# without .git); otherwise git is asked, else "unknown".
if(NOT CROCKET_GIT_COMMIT)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE CROCKET_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _crocket_git_rc)
    if(NOT _crocket_git_rc EQUAL 0)
      set(CROCKET_GIT_COMMIT "")
    endif()
  endif()
  if(NOT CROCKET_GIT_COMMIT)
    set(CROCKET_GIT_COMMIT "unknown")
  endif()
endif()
string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _crocket_cc)
if(_crocket_cc STREQUAL "gnu")
  set(_crocket_cc "gcc")
endif()
set(CROCKET_COMPILER "${_crocket_cc}-${CMAKE_CXX_COMPILER_VERSION}")
configure_file(cmake/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/crocket/version.hpp @ONLY)
target_include_directories(crocket PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(crocket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Users of crocket need C++26 and reflection too, so both are PUBLIC.
target_compile_features(crocket PUBLIC cxx_std_26)
target_compile_options(crocket PUBLIC $<$<CXX_COMPILER_ID:GNU>:-freflection>)
target_compile_options(crocket PRIVATE -Wall -Wextra)
# h2o is PRIVATE: application code never sees h2o headers. Only the engine
# includes them.
target_include_directories(crocket SYSTEM PRIVATE ${CROCKET_H2O_INCLUDE_DIRS})
target_compile_definitions(crocket PRIVATE ${CROCKET_H2O_DEFINITIONS})
target_link_libraries(crocket PRIVATE libh2o-evloop PUBLIC Threads::Threads)
if(CROCKET_FUZZ_COVERAGE)
  target_compile_options(crocket PRIVATE -fsanitize-coverage=trace-pc)
endif()

# ---- crocket::pg (optional): the crocket::db API on Postgres ------------------------------------------------------
# Plain structs in and out of Postgres through libpqxx. Built when libpqxx is
# found (CROCKET_PG=AUTO, the default); ON makes a missing libpqxx an error,
# OFF skips it. Core crocket never depends on it.
set(CROCKET_PG AUTO CACHE STRING "Build crocket::pg, the Postgres backend of crocket::db (AUTO, ON or OFF)")
set_property(CACHE CROCKET_PG PROPERTY STRINGS AUTO ON OFF)
set(CROCKET_HAS_PG OFF)
if(NOT CROCKET_PG STREQUAL "OFF")
  find_package(libpqxx CONFIG QUIET)
  if(TARGET libpqxx::pqxx)
    set(_crocket_pqxx libpqxx::pqxx)
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(CROCKET_PQXX QUIET IMPORTED_TARGET libpqxx)
      if(CROCKET_PQXX_FOUND)
        set(_crocket_pqxx PkgConfig::CROCKET_PQXX)
      endif()
    endif()
  endif()
  if(_crocket_pqxx)
    set(CROCKET_HAS_PG ON)
  elseif(CROCKET_PG STREQUAL "ON")
    message(FATAL_ERROR "crocket: CROCKET_PG=ON but libpqxx was not found (apt install libpqxx-dev)")
  endif()
endif()
if(CROCKET_HAS_PG)
  add_library(crocket_pg STATIC src/db.cpp)
  add_library(crocket::pg ALIAS crocket_pg)
  target_link_libraries(crocket_pg PUBLIC crocket ${_crocket_pqxx})
  target_compile_options(crocket_pg PRIVATE -Wall -Wextra)
  message(STATUS "crocket: crocket::pg enabled (libpqxx; the crocket::db API on Postgres)")
else()
  message(STATUS "crocket: crocket::pg disabled (no libpqxx)")
endif()

# ---- crocket::auth (optional): JWT / OIDC ---------------------------------------------------------------------
# A resource-server verifier. Built when OpenSSL 3 is found (CROCKET_AUTH=AUTO,
# the default); ON makes a missing OpenSSL an error, OFF skips it. Core crocket
# never links it, and crocket.hpp does not include the header.
set(CROCKET_AUTH AUTO CACHE STRING "Build crocket::auth (JWT/OIDC) (AUTO, ON or OFF)")
set_property(CACHE CROCKET_AUTH PROPERTY STRINGS AUTO ON OFF)
set(CROCKET_HAS_AUTH OFF)
if(NOT CROCKET_AUTH STREQUAL "OFF")
  find_package(OpenSSL 3.0)
  if(OpenSSL_FOUND)
    set(CROCKET_HAS_AUTH ON)
  elseif(CROCKET_AUTH STREQUAL "ON")
    message(FATAL_ERROR "crocket: CROCKET_AUTH=ON but OpenSSL 3 was not found")
  endif()
endif()
if(CROCKET_HAS_AUTH)
  add_library(crocket_auth STATIC src/auth/jwt.cpp src/auth/jwks.cpp src/auth/oidc.cpp)
  add_library(crocket::auth ALIAS crocket_auth)
  # libssl as well as libcrypto: the JWKS fetch is a small HTTPS client until
  # crocket has a resilience HTTP client to swap in.
  target_link_libraries(crocket_auth PUBLIC crocket PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_options(crocket_auth PRIVATE -Wall -Wextra)
  message(STATUS "crocket: crocket::auth enabled (OpenSSL; JWT/OIDC)")
else()
  message(STATUS "crocket: crocket::auth disabled")
endif()

if(CROCKET_BUILD_EXAMPLES)
  if(CROCKET_HAS_PG)
    add_executable(crocket_heroes examples/heroes.cpp)
    target_link_libraries(crocket_heroes PRIVATE crocket::pg)
  endif()
  add_executable(crocket_hello examples/hello.cpp)
  target_link_libraries(crocket_hello PRIVATE crocket)
  add_executable(crocket_serve examples/serve.cpp)
  target_link_libraries(crocket_serve PRIVATE crocket)
  add_executable(crocket_grpc examples/grpc.cpp)
  target_link_libraries(crocket_grpc PRIVATE crocket)
  if(CROCKET_HAS_AUTH)
    add_executable(crocket_auth_example examples/auth.cpp)
    target_link_libraries(crocket_auth_example PRIVATE crocket::auth)
  endif()
endif()

# Benchmarks (./dev bench). Built only on request, never by `all` or ctest.
add_executable(crocket_bench EXCLUDE_FROM_ALL bench/server.cpp)
target_link_libraries(crocket_bench PRIVATE crocket)
add_executable(crocket_microbench EXCLUDE_FROM_ALL bench/micro.cpp)
target_link_libraries(crocket_microbench PRIVATE crocket)
target_include_directories(crocket_microbench PRIVATE src)  # the router, measured directly
# h2o alone, answering the same scenarios from constants: crocket's floor (./dev bench --vs-h2o).
add_executable(crocket_bench_h2o EXCLUDE_FROM_ALL bench/h2o.cpp src/cpus.cpp)
target_include_directories(crocket_bench_h2o SYSTEM PRIVATE ${CROCKET_H2O_INCLUDE_DIRS})
target_include_directories(crocket_bench_h2o PRIVATE src)  # cpus.hpp: the same event loop count as crocket
target_compile_definitions(crocket_bench_h2o PRIVATE ${CROCKET_H2O_DEFINITIONS})
target_link_libraries(crocket_bench_h2o PRIVATE libh2o-evloop Threads::Threads)

if(CROCKET_HAS_PG)
  # crocket::db against hand-written libpqxx (./dev bench db). Needs Postgres.
  add_executable(crocket_bench_db EXCLUDE_FROM_ALL bench/db.cpp)
  target_link_libraries(crocket_bench_db PRIVATE crocket::pg)
endif()

if(CROCKET_HAS_AUTH)
  # me(Auth<>): no auth, cache hit, cache miss (./dev bench auth).
  add_executable(crocket_bench_auth EXCLUDE_FROM_ALL bench/auth.cpp)
  target_link_libraries(crocket_bench_auth PRIVATE crocket::auth OpenSSL::Crypto)
endif()

if(CROCKET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
