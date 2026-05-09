#!/usr/bin/env bash
# Создание нового C++ (Drogon) сервиса
# Использование: bash infra/new-service-cpp.sh <name>
# Пример:       bash infra/new-service-cpp.sh billing
set -euo pipefail

if [ -z "${1:-}" ]; then
  echo "Использование: $0 <name>"
  echo "Пример: $0 billing"
  exit 1
fi

NAME="$1"
SVC="service-${NAME}"
SVC_DIR="services/${SVC}"
NS="${NAME}_svc"
CLASS="$(echo "${NAME}" | sed -r 's/(^|_)([a-z])/\U\2/g')Controller"
BIN="service_${NAME}"

cd "$(dirname "$0")/.."

if [ -d "${SVC_DIR}" ]; then
  echo "ОШИБКА: ${SVC_DIR} уже существует"
  exit 1
fi

# --- 1. Структура директорий ---
echo "[1/8] Создаю структуру ${SVC_DIR}/..."
mkdir -p "${SVC_DIR}/include" "${SVC_DIR}/src/${NAME}" "${SVC_DIR}/migrations"

# --- 2. CMakeLists.txt ---
echo "[2/8] Генерирую CMakeLists.txt..."
cat > "${SVC_DIR}/CMakeLists.txt" << 'CMAKEOF'
cmake_minimum_required(VERSION 3.20)
project(__BIN__ CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Wall -Wextra -Wpedantic -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)

if(NOT TARGET common)
  set(COMMON_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/common"
      CACHE PATH "Path to libs/common")
  add_subdirectory(${COMMON_LIB_DIR} ${CMAKE_CURRENT_BINARY_DIR}/_common)
endif()

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS src/*.cc)

add_executable(__BIN__ ${SRC})

target_include_directories(__BIN__ PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(__BIN__ PRIVATE
  common
  Drogon::Drogon
  OpenSSL::SSL OpenSSL::Crypto
  PkgConfig::JWT
)
CMAKEOF
sed -i "s/__BIN__/${BIN}/g" "${SVC_DIR}/CMakeLists.txt"

# --- 3. Контроллер ---
echo "[3/8] Генерирую контроллер ${CLASS}..."
cat > "${SVC_DIR}/include/${NAME}_controller.h" << HEOF
#pragma once
#include <drogon/HttpController.h>

namespace ${NS} {

class ${CLASS} : public drogon::HttpController<${CLASS}> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(${CLASS}::health, "/health", drogon::Get);
  METHOD_LIST_END

  drogon::Task<>
  health(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);
};

} // namespace ${NS}
HEOF

# --- 4. Health handler ---
echo "[4/8] Генерирую health handler..."
cat > "${SVC_DIR}/src/${NAME}/health.cc" << CCEOF
#include "${NAME}_controller.h"
#include <drogon/drogon.h>

using namespace ${NS};
using namespace drogon;

Task<>
${CLASS}::health(HttpRequestPtr req,
                 std::function<void(const HttpResponsePtr &)> cb) {
  Json::Value out;
  out["service"] = "${SVC}";
  out["version"] = "0.1.0";

  bool pg_ok = false;
  try {
    auto db = drogon::app().getDbClient();
    co_await db->execSqlCoro("SELECT 1");
    pg_ok = true;
  } catch (...) {}

  out["ok"] = pg_ok;
  out["postgres_ok"] = pg_ok;
  cb(HttpResponse::newHttpJsonResponse(out));
  co_return;
}
CCEOF

# --- 5. main.cc ---
echo "[5/8] Генерирую main.cc..."
cat > "${SVC_DIR}/src/main.cc" << 'MAINEOF'
#include <drogon/drogon.h>

int main() {
  drogon::app().loadConfigFile("/app/config.json");
  drogon::app().run();
  return 0;
}
MAINEOF

# --- 6. config.json.tpl ---
echo "[6/8] Генерирую config.json.tpl..."
cat > "${SVC_DIR}/config.json.tpl" << TPLEOF
{
  "app": {
    "threads_num": 4,
    "log": { "log_level": "INFO" }
  },
  "listeners": [
    { "address": "0.0.0.0", "port": 8080, "https": false }
  ],
  "db_clients": [
    {
      "name": "default",
      "rdbms": "postgresql",
      "host": "\${DB_HOST}",
      "port": 5432,
      "dbname": "\${DB_NAME}",
      "user": "\${DB_USER}",
      "passwd": "\${DB_PASS}",
      "is_fast": false,
      "connection_number": 8
    }
  ]
}
TPLEOF

# --- 7. Dockerfile ---
echo "[7/8] Генерирую Dockerfile..."
cat > "${SVC_DIR}/Dockerfile" << DEOF
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \\
    build-essential cmake pkg-config git ca-certificates autoconf automake libtool \\
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \\
    libpq-dev libhiredis-dev libjansson-dev libgnutls28-dev \\
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch v3.2.3 https://github.com/benmcollins/libjwt.git /tmp/libjwt \\
    && cd /tmp/libjwt && mkdir build && cd build \\
    && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. \\
    && make -j\$(nproc) && make install \\
    && ldconfig && rm -rf /tmp/libjwt

RUN git clone --depth 1 --branch v1.9.7 https://github.com/drogonframework/drogon.git /tmp/drogon \\
    && cd /tmp/drogon && git submodule update --init \\
    && mkdir build && cd build \\
    && cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_POSTGRESQL=ON -DBUILD_REDIS=ON .. \\
    && make -j\$(nproc) && make install \\
    && rm -rf /tmp/drogon

WORKDIR /app
COPY libs/common/ /app/libs/common/
COPY services/${SVC}/ /app/

RUN rm -rf build && mkdir build && cd build \\
    && cmake -DCMAKE_BUILD_TYPE=Release -DCOMMON_LIB_DIR=/app/libs/common .. \\
    && make -j\$(nproc)

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \\
    libjsoncpp25 libssl3 libpq5 libhiredis0.14 \\
    libuuid1 zlib1g libjansson4 libgnutls30 gettext-base \\
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libjwt* /usr/local/lib/
COPY --from=build /usr/local/lib/libdrogon* /usr/local/lib/
COPY --from=build /usr/local/lib/libtrantor* /usr/local/lib/
RUN ldconfig

COPY --from=build /app/build/${BIN} /usr/local/bin/${BIN}
COPY services/${SVC}/config.json.tpl /app/config.json.tpl
COPY infra/entrypoint.sh /app/entrypoint.sh

EXPOSE 8080
WORKDIR /app
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["/usr/local/bin/${BIN}"]
DEOF

# --- 8. Helm values ---
echo "[8/8] Генерирую Helm values..."
cat > "infra/helm/values-${SVC}.yaml" << VEOF
replicaCount: 1

image:
  repository: localhost:5000/${SVC}
  tag: latest

service:
  port: 8080

config:
  enabled: true
  json:
    app:
      threads_num: 4
      log:
        log_level: INFO
    listeners:
      - address: "0.0.0.0"
        port: 8080
        https: false
    db_clients:
      - name: default
        rdbms: postgresql
        host: pg-${NAME}-postgresql.data.svc.cluster.local
        port: 5432
        dbname: ${NAME}
        user: ${NAME}
        passwd: ${NAME}
        is_fast: false
        connection_number: 8

jwtKeys:
  private: false
  public: true
  publicFile: jwt_public.jwk
  mountPath: /etc/keys

env:
  - name: JWT_PUBLIC_KEY_PATH
    value: /etc/keys/jwt_public.jwk

migrations:
  enabled: true
  image: postgres:16
  files:
    001_init.sql: |
      CREATE TABLE IF NOT EXISTS _placeholder (id SERIAL PRIMARY KEY);
  db:
    host: pg-${NAME}-postgresql.data.svc.cluster.local
    port: "5432"
    name: ${NAME}
    user: ${NAME}
    password: ${NAME}

ingress:
  enabled: true
  match: "PathPrefix(\`/api/${NAME}\`)"

cors:
  enabled: true

networkPolicy:
  enabled: true

healthcheck:
  path: /health
VEOF

# --- 9. Обновляю Makefile ---
echo ""
echo "[+] Обновляю K3S_SERVICES в Makefile..."
sed -i "s/^K3S_SERVICES := .*/& ${SVC}/" Makefile

echo "[+] Обновляю k3s-deploy-data в Makefile..."
sed -i "/helm upgrade --install redis/i\\
\\thelm upgrade --install pg-${NAME} oci://registry-1.docker.io/bitnamicharts/postgresql \\\\\\n\\t\\t-n data \\\\\\n\\t\\t--set auth.username=${NAME} --set auth.password=${NAME} --set auth.database=${NAME} \\\\\\n\\t\\t--set primary.persistence.size=1Gi" Makefile

echo "[+] Обновляю down-k3s в Makefile..."
sed -i "s/\(-helm uninstall.*-n backend\)/\1/" Makefile
sed -i "s/\(-helm uninstall .* -n backend\)/\\1/" Makefile

# --- 10. Обновляю корневой CMakeLists.txt ---
echo "[+] Обновляю CMakeLists.txt..."
if ! grep -q "services/${SVC}" CMakeLists.txt; then
  sed -i "/add_subdirectory(services\/service-core)/a add_subdirectory(services/${SVC})" CMakeLists.txt
fi

# --- 11. Миграция-заглушка ---
echo "[+] Создаю миграцию-заглушку..."
cat > "${SVC_DIR}/migrations/001_init.sql" << 'SQLEOF'
CREATE TABLE IF NOT EXISTS _placeholder (id SERIAL PRIMARY KEY);
SQLEOF

echo ""
echo "=== Готово: ${SVC} (C++ / Drogon) ==="
echo ""
echo "Файлы:"
echo "  ${SVC_DIR}/include/${NAME}_controller.h"
echo "  ${SVC_DIR}/src/${NAME}/health.cc"
echo "  ${SVC_DIR}/src/main.cc"
echo "  ${SVC_DIR}/CMakeLists.txt"
echo "  ${SVC_DIR}/Dockerfile"
echo "  ${SVC_DIR}/config.json.tpl"
echo "  infra/helm/values-${SVC}.yaml"
echo ""
echo "Следующие шаги:"
echo "  1. Отредактируй migrations/001_init.sql"
echo "  2. Добавь ручки в include/${NAME}_controller.h"
echo "  3. make k3s-build-${SVC} && make k3s-deploy-${SVC}"
