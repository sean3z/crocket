# crocket's toolchain: Ubuntu 26.04, the ./dev setup packages, GCC 16.2 from
# ./dev gcc, and libpqxx built with that GCC in C++26 (for crocket::pg: libpqxx's
# ABI follows the compiler and standard it is built with, so the distro build
# cannot be mixed with crocket's).
# Published as ghcr.io/sean3z/crocket-toolchain:gcc-16.2 and :gcc-16.2-pg
# (the same image) by .github/workflows/toolchain.yml.
#
# A downstream CMake build needs nothing else from the image: gcc/cc and g++
# are GCC 16.2, h2o's build dependencies and libpqxx are installed, and a
# binary gets an rpath to GCC 16.2's libstdc++. `/opt/gcc-16.2/runtime` holds
# the real libstdc++.so.6 and libgcc_s.so.1 for a slim runtime image, which
# copies that directory onto the rpath and installs the shared libraries the
# binary still links (OpenSSL, zlib, and libpq for crocket::pg).
# See README "Deployment".
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
 && ln -sfn gcc-16.2 /opt/gcc-16.2/bin/gcc \
 && ln -sfn gcc-16.2 /opt/gcc-16.2/bin/cc \
 && ln -sfn g++-16.2 /opt/gcc-16.2/bin/g++ \
 && ln -sfn g++-16.2 /opt/gcc-16.2/bin/c++ \
 && echo /opt/gcc-16.2/lib64 > /etc/ld.so.conf.d/gcc-16.2.conf \
 && ldconfig \
 && rm -rf /tmp/crocket /root/.cache/crocket-gcc /var/lib/apt/lists/*
# gcc-16.2.conf sorts before libc.conf, so this libstdc++ is the one the
# loader finds. LDFLAGS gives CMake binaries the same directory as an rpath,
# which a runtime image can satisfy by copying the two shared libraries there.
ENV PATH=/opt/gcc-16.2/bin:$PATH \
    CC=/opt/gcc-16.2/bin/gcc-16.2 \
    CXX=/opt/gcc-16.2/bin/g++-16.2 \
    LDFLAGS="-Wl,-rpath,/opt/gcc-16.2/lib64"

FROM ${BASE} AS toolchain
ARG PQXX_VERSION=7.10.0
# Repeated for a BASE image built before these were set. PATH is set to the
# full value so a base that already prepends /opt/gcc-16.2/bin does not double it.
ENV CROCKET_GCC_PREFIX=/opt/gcc-16.2 \
    DEBIAN_FRONTEND=noninteractive \
    PATH=/opt/gcc-16.2/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    CC=/opt/gcc-16.2/bin/gcc-16.2 \
    CXX=/opt/gcc-16.2/bin/g++-16.2 \
    LDFLAGS="-Wl,-rpath,/opt/gcc-16.2/lib64"
RUN ln -sfn gcc-16.2 /opt/gcc-16.2/bin/gcc \
 && ln -sfn gcc-16.2 /opt/gcc-16.2/bin/cc \
 && ln -sfn g++-16.2 /opt/gcc-16.2/bin/g++ \
 && ln -sfn g++-16.2 /opt/gcc-16.2/bin/c++ \
 && echo /opt/gcc-16.2/lib64 > /etc/ld.so.conf.d/gcc-16.2.conf \
 && ldconfig \
 && mkdir -p /opt/gcc-16.2/runtime \
 && cp -L /opt/gcc-16.2/lib64/libstdc++.so.6 /opt/gcc-16.2/runtime/libstdc++.so.6 \
 && cp -L /opt/gcc-16.2/lib64/libgcc_s.so.1 /opt/gcc-16.2/runtime/libgcc_s.so.1 \
 && apt-get update -q \
 && apt-get install -y -q --no-install-recommends \
      libssl-dev zlib1g-dev pkg-config \
      libpq-dev postgresql-client \
 && curl -fsSL https://github.com/jtv/libpqxx/archive/refs/tags/${PQXX_VERSION}.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/libpqxx-${PQXX_VERSION} -B /tmp/pqxx-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_STANDARD=26 -DSKIP_BUILD_TEST=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_DOC=OFF \
 && cmake --build /tmp/pqxx-build \
 && cmake --install /tmp/pqxx-build \
 && rm -rf /tmp/libpqxx-* /tmp/pqxx-build /var/lib/apt/lists/*
WORKDIR /src
