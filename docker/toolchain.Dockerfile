# crocket's toolchain: Ubuntu 26.04, the ./dev setup packages, GCC 16.2 from
# ./dev gcc, and libpqxx built with that GCC in C++26 (for crocket::pg: libpqxx's
# ABI follows the compiler and standard it is built with, so the distro build
# cannot be mixed with crocket's).
# Published as ghcr.io/sean3z/crocket-toolchain:gcc-16.2-pg by .github/workflows/toolchain.yml.
#
# BASE picks where GCC comes from: the gcc stage below (the default, about an
# hour), or an already published image with it (toolchain.yml's reuse_gcc), so
# adding a library does not rebuild the compiler.
ARG BASE=gcc

FROM ubuntu:26.04 AS gcc
ENV CROCKET_GCC_PREFIX=/opt/gcc-16.2 \
    DEBIAN_FRONTEND=noninteractive
COPY dev /tmp/crocket/dev
RUN /tmp/crocket/dev setup \
 && /tmp/crocket/dev gcc \
 && rm -rf /tmp/crocket /root/.cache/crocket-gcc /var/lib/apt/lists/*
ENV PATH=/opt/gcc-16.2/bin:$PATH

FROM ${BASE} AS toolchain
ARG PQXX_VERSION=7.10.0
ENV CROCKET_GCC_PREFIX=/opt/gcc-16.2 \
    DEBIAN_FRONTEND=noninteractive \
    PATH=/opt/gcc-16.2/bin:$PATH
RUN apt-get update -q \
 && apt-get install -y -q --no-install-recommends libpq-dev postgresql-client \
 && curl -fsSL https://github.com/jtv/libpqxx/archive/refs/tags/${PQXX_VERSION}.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/libpqxx-${PQXX_VERSION} -B /tmp/pqxx-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=/opt/gcc-16.2/bin/g++-16.2 -DCMAKE_C_COMPILER=/opt/gcc-16.2/bin/gcc-16.2 \
      -DCMAKE_CXX_STANDARD=26 -DSKIP_BUILD_TEST=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_DOC=OFF \
 && cmake --build /tmp/pqxx-build \
 && cmake --install /tmp/pqxx-build \
 && rm -rf /tmp/libpqxx-* /tmp/pqxx-build /var/lib/apt/lists/*
WORKDIR /src
