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

# ---- h2o (the only wire engine) ---------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/h2o.cmake)

# ---- crocket ---------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)  # gzip responses; h2o needs it too

add_library(crocket STATIC
  src/app.cpp
  src/compress.cpp
  src/conditional.cpp
  src/identity.cpp
  src/router.cpp
  src/json.cpp
  src/http.cpp
  src/fairings.cpp
  src/grpc.cpp
  src/engine_h2o.cpp)
add_library(crocket::crocket ALIAS crocket)
target_include_directories(crocket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Users of crocket need C++26 and reflection too, so both are PUBLIC.
target_compile_features(crocket PUBLIC cxx_std_26)
target_compile_options(crocket PUBLIC -freflection)
target_compile_options(crocket PRIVATE -Wall -Wextra)
# h2o is PRIVATE: application code never sees h2o headers. Only the engine
# includes them.
target_include_directories(crocket SYSTEM PRIVATE ${CROCKET_H2O_INCLUDE_DIRS})
target_compile_definitions(crocket PRIVATE ${CROCKET_H2O_DEFINITIONS})
target_link_libraries(crocket PRIVATE libh2o-evloop ZLIB::ZLIB PUBLIC Threads::Threads)

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
