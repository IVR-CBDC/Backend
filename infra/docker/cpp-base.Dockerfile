# Общий базовый образ для C++/Drogon-сервисов (service-auth, service-core).
#
# Зачем: сборка drogon из исходников занимает ~15 минут. Раньше каждый
# сервисный Dockerfile собирал libjwt и drogon самостоятельно — CI гонял
# эту сборку дважды на каждый PR. Здесь она делается один раз, образ
# ivr-cpp-base:latest кэшируется, а сервисные Dockerfile'ы стартуют от него.
#
# Образ не зависит от исходников репозитория (ничего из libs/ и services/
# сюда не копируется) — пересобирается только при смене версий зависимостей
# ниже, а не на каждое изменение кода сервисов.
#
# При смене версии libjwt/drogon (или списка пакетов) — пересобрать:
#   make cpp-base
#
# F6 (final review): версия Debian здесь (bookworm) — контракт с runtime-
# стадиями services/service-auth/Dockerfile и services/service-core/Dockerfile
# (debian:bookworm-slim + точные soname-пакеты libjsoncpp25/libhiredis0.14/
# libssl3/...). При бампе версии здесь — менять все три места синхронно и
# проверять `ldd` на собранный бинарь, иначе libdrogon*, скопированный из
# новой build-стадии, слинкуется с другими so, чем те, что ставит runtime-
# стадия.
FROM debian:bookworm AS build

# Объединение пакетов service-auth и service-core; libargon2-dev нужен
# только auth (хэширование паролей), но он безвреден для core.
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential cmake pkg-config git ca-certificates autoconf automake libtool \
	libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
	libpq-dev libhiredis-dev libargon2-dev libjansson-dev libgnutls28-dev \
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
	&& ldconfig && rm -rf /tmp/drogon
