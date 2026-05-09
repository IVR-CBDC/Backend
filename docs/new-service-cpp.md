# Создание нового C++ сервиса (Drogon)

## 1. Структура

Создай директорию `services/service-<name>/` со следующей структурой:

```
services/service-<name>/
  CMakeLists.txt
  Dockerfile
  config.json.tpl
  include/
    <name>_controller.h
  src/
    main.cc
    <name>/
      health.cc
      <your_handler>.cc
  migrations/
    001_init.sql          # (опционально)
```

## 2. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(service_<name> CXX)

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

add_executable(service_<name> ${SRC})

target_include_directories(service_<name> PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(service_<name> PRIVATE
  common
  Drogon::Drogon
  OpenSSL::SSL OpenSSL::Crypto
  PkgConfig::JWT
)
```

## 3. Контроллер (include/<name>_controller.h)

```cpp
#pragma once
#include <drogon/HttpController.h>

namespace <name>_svc {

class <Name>Controller : public drogon::HttpController<<Name>Controller> {
public:
  METHOD_LIST_BEGIN

  // Healthcheck (обязателен)
  ADD_METHOD_TO(<Name>Controller::health, "/health", drogon::Get);

  // Пример публичной ручки
  ADD_METHOD_TO(<Name>Controller::myEndpoint, "/api/<name>/my-endpoint", drogon::Post);

  // Пример защищённой ручки (JWT)
  ADD_METHOD_TO(<Name>Controller::protectedEndpoint, "/api/<name>/protected",
                drogon::Get, "common::JwtFilter");

  METHOD_LIST_END

  drogon::Task<>
  health(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);

  drogon::Task<>
  myEndpoint(drogon::HttpRequestPtr req,
             std::function<void(const drogon::HttpResponsePtr &)> cb);

  drogon::Task<>
  protectedEndpoint(drogon::HttpRequestPtr req,
                    std::function<void(const drogon::HttpResponsePtr &)> cb);
};

} // namespace <name>_svc
```

Ключевые моменты:
- Путь роута всегда начинается с `/api/<name>/`
- Для JWT-защиты добавь фильтр `"common::JwtFilter"` последним аргументом в `ADD_METHOD_TO`
- `/health` всегда без префикса

## 4. Реализация хендлера (src/<name>/my_endpoint.cc)

```cpp
#include "<name>_controller.h"
#include <drogon/drogon.h>

using namespace <name>_svc;
using namespace drogon;

Task<>
<Name>Controller::myEndpoint(HttpRequestPtr req,
                             std::function<void(const HttpResponsePtr &)> cb) {
  auto json = req->getJsonObject();
  if (!json) {
    auto resp = HttpResponse::newHttpJsonResponse(
        Json::Value{Json::objectValue});
    resp->setStatusCode(k400BadRequest);
    (*resp->getJsonObject())["error"] = "invalid json";
    cb(resp);
    co_return;
  }

  // Работа с БД
  auto db = drogon::app().getDbClient();
  auto result = co_await db->execSqlCoro(
      "SELECT id, value FROM my_table WHERE id = $1",
      (*json)["id"].asString());

  Json::Value out;
  out["count"] = (int)result.size();
  cb(HttpResponse::newHttpJsonResponse(out));
  co_return;
}
```

Для защищённых ручек user_id доступен через:
```cpp
auto user_id = req->attributes()->get<std::string>("user_id");
```

## 5. Health check (src/<name>/health.cc)

```cpp
#include "<name>_controller.h"
#include <drogon/drogon.h>

using namespace <name>_svc;
using namespace drogon;

Task<>
<Name>Controller::health(HttpRequestPtr req,
                         std::function<void(const HttpResponsePtr &)> cb) {
  Json::Value out;
  out["service"] = "service-<name>";
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
```

## 6. main.cc

```cpp
#include <drogon/drogon.h>

int main() {
  drogon::app().loadConfigFile("/app/config.json");
  drogon::app().run();
  return 0;
}
```

## 7. config.json.tpl

```json
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
      "host": "${DB_HOST}",
      "port": 5432,
      "dbname": "${DB_NAME}",
      "user": "${DB_USER}",
      "passwd": "${DB_PASS}",
      "is_fast": false,
      "connection_number": 8
    }
  ]
}
```

## 8. Dockerfile

Скопируй `services/service-core/Dockerfile` и замени `service-core` / `service_core` на своё имя.

## 9. Helm values (infra/helm/values-service-<name>.yaml)

```yaml
replicaCount: 1

image:
  repository: localhost:5000/service-<name>
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
        host: pg-<name>-postgresql.data.svc.cluster.local
        port: 5432
        dbname: <name>
        user: <name>
        passwd: <name>
        is_fast: false
        connection_number: 8

jwtKeys:
  private: false       # true если сервис выпускает токены
  public: true
  publicFile: jwt_public.jwk
  mountPath: /etc/keys

env:
  - name: JWT_PUBLIC_KEY_PATH
    value: /etc/keys/jwt_public.jwk

migrations:
  enabled: true        # false если нет миграций
  image: postgres:16
  files:
    001_init.sql: |
      CREATE TABLE IF NOT EXISTS my_table (
          id UUID PRIMARY KEY,
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
      );
  db:
    host: pg-<name>-postgresql.data.svc.cluster.local
    port: "5432"
    name: <name>
    user: <name>
    password: <name>

ingress:
  enabled: true
  match: "PathPrefix(`/api/<name>`)"

cors:
  enabled: true

networkPolicy:
  enabled: true

healthcheck:
  path: /health
```

## 10. Регистрация в Makefile

В `K3S_SERVICES` добавь имя:
```makefile
K3S_SERVICES := service-auth service-core service-test-python service-<name>
```

В `k3s-deploy-data` добавь PG:
```makefile
helm upgrade --install pg-<name> oci://registry-1.docker.io/bitnamicharts/postgresql \
    -n data \
    --set auth.username=<name> --set auth.password=<name> --set auth.database=<name> \
    --set primary.persistence.size=1Gi
```

В корневой `CMakeLists.txt` добавь:
```cmake
add_subdirectory(services/service-<name>)
```

## 11. Добавление ручки в существующий сервис

1. В `include/<name>_controller.h` внутри `METHOD_LIST_BEGIN...END` добавь:
   ```cpp
   ADD_METHOD_TO(<Name>Controller::newHandler, "/api/<name>/new-route", drogon::Post);
   ```
   И объявление метода ниже:
   ```cpp
   drogon::Task<>
   newHandler(drogon::HttpRequestPtr req,
              std::function<void(const drogon::HttpResponsePtr &)> cb);
   ```

2. Создай `src/<name>/new_handler.cc` с реализацией (см. пример в п.4)

3. Пересобери: `make k3s-build-service-<name> && make k3s-deploy-service-<name>`

Новый `.cc` файл подхватится автоматически через `GLOB_RECURSE` в CMakeLists.
