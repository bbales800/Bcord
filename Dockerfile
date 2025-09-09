# ---- Base ------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---- OS deps: compiler, cmake, ninja, libs we actually use -----------------
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates git curl pkg-config \
      build-essential cmake ninja-build \
      libssl-dev \
      libpq-dev \
      libboost-dev libboost-system-dev libboost-thread-dev \
      libhiredis-dev \
      nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*


# ---- Build redis-plus-plus (headers+libs into /usr/local) ------------------
# Pin to a known release to keep cache stable; adjust tag if needed.
ARG RPP_TAG=1.3.15
RUN git clone --depth 1 --branch ${RPP_TAG} https://github.com/sewenew/redis-plus-plus.git /tmp/redis-plus-plus && \
    cmake -G Ninja -S /tmp/redis-plus-plus -B /tmp/redis-plus-plus/build \
      -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
      -DREDIS_PLUS_PLUS_BUILD_EXAMPLES=OFF \
      -DREDIS_PLUS_PLUS_BUILD_BENCHMARK=OFF && \
    cmake --build /tmp/redis-plus-plus/build -j && \
    cmake --install /tmp/redis-plus-plus/build && \
    ldconfig && \
    rm -rf /tmp/redis-plus-plus

# ---- Build libpqxx from source so headers == library -----------------------
# Pin to a stable tag; adjust if desired.
ARG PQXX_TAG=7.8.1
RUN git clone --depth 1 --branch ${PQXX_TAG} https://github.com/jtv/libpqxx.git /tmp/libpqxx && \
    cmake -G Ninja -S /tmp/libpqxx -B /tmp/libpqxx/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/libpqxx/build -j && \
    cmake --install /tmp/libpqxx/build && \
    ldconfig && \
    rm -rf /tmp/libpqxx

# ---- App -------------------------------------------------------------------
WORKDIR /app
COPY . /app

# Define this globally as a fallback (we also set it in CMakeLists)
ENV CXXFLAGS="${CXXFLAGS} -DPQXX_HIDE_SOURCE_LOCATION"

# ---- Build server with CMake/Ninja -----------------------------------------
RUN rm -rf build && \
    cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local && \
    cmake --build build -j

# ---- Runtime ---------------------------------------------------------------
CMD ["/app/build/server/BCordServer"]
