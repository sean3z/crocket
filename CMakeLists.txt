cmake_minimum_required(VERSION 3.28)
project(crocket VERSION 0.1.0 LANGUAGES C CXX)

# Examples and tests are for working on crocket; a project that pulls crocket
# in with FetchContent or add_subdirectory doesn't build them.
option(CROCKET_BUILD_EXAMPLES "Build crocket_hello, crocket_serve and crocket_grpc" ${PROJECT_IS_TOP_LEVEL})
option(CROCKET_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

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

# ---- libwebsockets (the only wire engine) ---------------------------------
include(FetchContent)
# lws 4.3.5 declares an old cmake_minimum_required that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# lws's settings are plain variables in this directory, not cache entries, so
# they don't leak into a consumer's cache. CMP0077 lets option() honour them
# despite lws's old policy version.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(LWS_WITH_HTTP2 ON)
set(LWS_WITH_SSL ON)
set(LWS_WITH_HTTP_UNCOMMON_HEADERS ON)
set(LWS_WITH_CUSTOM_HEADERS ON)
set(LWS_WITH_SHARED OFF)
set(LWS_WITH_STATIC ON)
set(LWS_WITHOUT_TESTAPPS ON)
set(LWS_WITHOUT_TEST_SERVER ON)
set(LWS_WITHOUT_TEST_SERVER_EXTPOLL ON)
set(LWS_WITHOUT_TEST_PING ON)
set(LWS_WITHOUT_TEST_CLIENT ON)
set(LWS_WITH_MINIMAL_EXAMPLES OFF)
set(LWS_WITHOUT_EXTENSIONS ON)
set(LWS_WITH_EXPORT_LWSTARGETS OFF)
set(DISABLE_WERROR ON)
FetchContent_Declare(libwebsockets
  GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
  GIT_TAG v4.3.5
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(libwebsockets)

# ---- crocket ---------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(crocket STATIC
  src/app.cpp
  src/router.cpp
  src/json.cpp
  src/http.cpp
  src/fairings.cpp
  src/grpc.cpp
  src/engine_lws.cpp)
add_library(crocket::crocket ALIAS crocket)
target_include_directories(crocket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Users of crocket need C++26 and reflection too, so both are PUBLIC.
target_compile_features(crocket PUBLIC cxx_std_26)
target_compile_options(crocket PUBLIC -freflection)
target_compile_options(crocket PRIVATE -Wall -Wextra)
# lws is PRIVATE: application code never sees libwebsockets headers.
target_link_libraries(crocket PRIVATE websockets PUBLIC Threads::Threads)

if(CROCKET_BUILD_EXAMPLES)
  add_executable(crocket_hello examples/hello.cpp)
  target_link_libraries(crocket_hello PRIVATE crocket)
  add_executable(crocket_serve examples/serve.cpp)
  target_link_libraries(crocket_serve PRIVATE crocket)
  add_executable(crocket_grpc examples/grpc.cpp)
  target_link_libraries(crocket_grpc PRIVATE crocket)
endif()

if(CROCKET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
