FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates autoconf automake libtool \
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
    libpq-dev libhiredis-dev libjansson-dev libgnutls28-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch v3.2.3 https://github.com/benmcollins/libjwt.git /tmp/libjwt \
    && cd /tmp/libjwt && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. \
    && make -j$(nproc) && make install \
    && ldconfig && rm -rf /tmp/libjwt

RUN git clone --depth 1 --branch v1.9.7 https://github.com/drogonframework/drogon.git /tmp/drogon \
    && cd /tmp/drogon && git submodule update --init \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_POSTGRESQL=ON -DBUILD_REDIS=ON .. \
    && make -j$(nproc) && make install \
    && rm -rf /tmp/drogon

WORKDIR /app
COPY libs/common/ /app/libs/common/
COPY services/service-core/ /app/

RUN rm -rf build && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release -DCOMMON_LIB_DIR=/app/libs/common .. \
    && make -j$(nproc)

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libjsoncpp25 libssl3 libpq5 libhiredis0.14 \
    libuuid1 zlib1g libjansson4 libgnutls30 gettext-base \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libjwt* /usr/local/lib/

COPY --from=build /usr/local/lib/libdrogon* /usr/local/lib/
COPY --from=build /usr/local/lib/libtrantor* /usr/local/lib/
RUN ldconfig

COPY --from=build /app/build/service_core /usr/local/bin/service_core
COPY services/service-core/config.json.tpl /app/config.json.tpl
COPY infra/entrypoint.sh /app/entrypoint.sh

EXPOSE 8080
WORKDIR /app
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["/usr/local/bin/service_core"]
