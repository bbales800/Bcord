FROM ubuntu:24.04

# Base tools + CA certs (needed for git/wget TLS)
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates software-properties-common && \
    update-ca-certificates

# Build deps + runtime libs
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git wget \
      libboost-all-dev libpqxx-dev nlohmann-json3-dev \
      redis-server libhiredis-dev libargon2-dev && \
    rm -rf /var/lib/apt/lists/*

# (rest unchanged: build hiredis/redis++ from source, build your app, etc.)

# ---- build hiredis (provides hiredisConfig.cmake) ----
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates git && \
    update-ca-certificates && \
    git clone --depth 1 https://github.com/redis/hiredis.git /tmp/hiredis && \
    cmake -S /tmp/hiredis -B /tmp/hiredis/build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/hiredis/build -j && \
    cmake --install /tmp/hiredis/build && \
    ldconfig && \
    rm -rf /tmp/hiredis

# ---- build redis-plus-plus (redis++) ----
RUN git clone --depth 1 https://github.com/sewenew/redis-plus-plus.git /tmp/redis-plus-plus && \
    cmake -S /tmp/redis-plus-plus -B /tmp/redis-plus-plus/build \
      -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
      -DREDIS_PLUS_PLUS_BUILD_SHARED=ON \
      -DREDIS_PLUS_PLUS_CXX_STANDARD=17 \
      -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/redis-plus-plus/build -j && \
    cmake --install /tmp/redis-plus-plus/build && \
    ldconfig && \
    rm -rf /tmp/redis-plus-plus

# ---- your app ----
WORKDIR /app
COPY . /app

RUN rm -rf build && \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local && \
    cmake --build build -j

CMD ["./build/server/BCordServer"]
