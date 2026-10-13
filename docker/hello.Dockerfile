# crocket's hello example, ready to run:
#   docker run -p 8000:8000 ghcr.io/sean3z/crocket-hello:stable
#   curl localhost:8000/hello/Rocketeer/42
# It runs in the toolchain image, so GCC 16.2's libstdc++ is there at run time.
FROM ghcr.io/sean3z/crocket-toolchain:gcc-16.2
# The commit for crocket_build_info (.git is not in the build context).
ARG CROCKET_GIT_COMMIT=unknown
COPY . /src
RUN cmake --preset release -DCROCKET_GIT_COMMIT=${CROCKET_GIT_COMMIT} -DCMAKE_C_COMPILER=/opt/gcc-16.2/bin/gcc-16.2 \
      -DCMAKE_CXX_COMPILER=/opt/gcc-16.2/bin/g++-16.2 \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,$(dirname $(/opt/gcc-16.2/bin/g++-16.2 -print-file-name=libstdc++.so))" \
 && cmake --build build --target crocket_hello \
 && install -m755 "$(find build -type f -name crocket_hello -perm -u+x | head -1)" /usr/local/bin/ \
 && rm -rf /src/build
EXPOSE 8000
CMD ["crocket_hello"]
