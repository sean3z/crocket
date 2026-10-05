# h2o (https://github.com/h2o/h2o), crocket's wire engine: HTTP/1.1, HTTP/2 and
# TLS, built from h2o's own CMake as the static library libh2o-evloop.
#
# h2o has had no release tag since 2.2.6 (2019); Fastly ships its master branch,
# so crocket pins a master commit. To move to the newest h2o:
#
#   ./dev h2o-update            # latest master
#   ./dev h2o-update <ref>      # a branch, tag or commit
#
# rewrites the two lines below, then builds and runs the full test suite. To
# build offline, point FETCHCONTENT_SOURCE_DIR_H2O at an h2o checkout.
set(CROCKET_H2O_COMMIT cac7e6568ad98a848f099ecd0a18b881f632479a)  # 2026-09-10
set(CROCKET_H2O_SHA256 be4a7792211ab0d70513e5a2eea4e360b223dd1f8e43af4eacac214d1fba011b)

include(FetchContent)

# h2o's options are plain variables inside this block, so they reach h2o's
# option() calls without becoming cache entries in crocket's (or a consumer's)
# build. Everything crocket does not use is off: mruby, QUIC/HTTP/3 extras,
# compression codecs, io_uring, dtrace and the shared library.
block(SCOPE_FOR VARIABLES)
  set(BUILD_SHARED_LIBS OFF)
  set(WITHOUT_LIBS OFF)
  set(DISABLE_LIBUV ON)
  set(WITH_MRUBY OFF)
  set(WITH_CCACHE OFF)  # crocket's own compiler launcher applies instead
  set(WITH_DTRACE OFF)
  set(WITH_FUSION OFF)
  set(WITH_KTLS OFF)
  set(WITH_AEGIS OFF)
  set(WITH_IO_URING OFF)
  set(WITH_BROTLI OFF)
  set(WITH_ZSTD OFF)
  set(WITH_MPTCP OFF)
  # Optional packages h2o would pick up if installed; crocket uses none of them.
  set(CMAKE_DISABLE_FIND_PACKAGE_LIBUV TRUE)
  set(CMAKE_DISABLE_FIND_PACKAGE_WSLAY TRUE)
  set(CMAKE_DISABLE_FIND_PACKAGE_aegis TRUE)
  # h2o puts -g3 (debug info with every macro) first in its C flags; a later -g
  # wins and keeps libh2o-evloop's debug info to the normal size. Its warnings
  # (mostly variables used only by asserts in release builds) are not ours to fix.
  # Per-function sections let an application linking with -Wl,--gc-sections drop
  # the parts of h2o crocket never calls (HTTP/3, proxying, file serving).
  string(APPEND CMAKE_C_FLAGS " -g -w -ffunction-sections -fdata-sections")
  # h2o answers a request with more headers than this itself (400, connection
  # closed). Above crocket's LaunchOptions::max_header_count, so crocket sends
  # the 431 instead. Code including h2o.h gets the same value (below).
  string(APPEND CMAKE_C_FLAGS " -DH2O_MAX_HEADERS=128")
  FetchContent_Declare(h2o
    URL https://github.com/h2o/h2o/archive/${CROCKET_H2O_COMMIT}.tar.gz
    URL_HASH SHA256=${CROCKET_H2O_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
  FetchContent_MakeAvailable(h2o)
endblock()
FetchContent_GetProperties(h2o)

# h2o declares its include directories per directory, not on libh2o-evloop, and
# compiles the library with H2O_USE_LIBUV=0. Code that includes h2o.h needs the
# same, or its structs would not match the library's.
set(CROCKET_H2O_INCLUDE_DIRS
  ${h2o_SOURCE_DIR}/include
  ${h2o_SOURCE_DIR}/deps/picotls/include
  ${h2o_SOURCE_DIR}/deps/quicly/include)
set(CROCKET_H2O_DEFINITIONS H2O_USE_LIBUV=0 H2O_MAX_HEADERS=128)
