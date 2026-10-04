# A project that uses crocket the way an application would. tests/consumer.cmake
# configures, builds and runs it. It sets no C++ standard on purpose: linking
# crocket must be enough.
cmake_minimum_required(VERSION 3.28)
project(crocket_consumer CXX)

include(FetchContent)
FetchContent_Declare(crocket SOURCE_DIR ${CROCKET_SOURCE_DIR})
FetchContent_MakeAvailable(crocket)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE crocket::crocket)

# crocket's own tests, examples and settings stay out of the consumer's build.
foreach(t crocket_acceptance crocket_hello crocket_serve)
  if(TARGET ${t})
    message(FATAL_ERROR "crocket leaked target ${t} into the consumer")
  endif()
endforeach()
foreach(v WITH_MRUBY WITHOUT_LIBS DISABLE_LIBUV BUILD_SHARED_LIBS CROCKET_LINKER_lld_WORKS CROCKET_LINKER_mold_WORKS)
  if(DEFINED CACHE{${v}})
    message(FATAL_ERROR "crocket leaked cache entry ${v} into the consumer")
  endif()
endforeach()
