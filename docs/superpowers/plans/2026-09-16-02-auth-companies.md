# service-auth: компании, company_id в JWT, /me — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Привязать пользователя к компании (юрлицу), положить `company_id` в JWT, отдать данные профиля через `GET /api/auth/me` и завести в репозитории C++-тесты (Catch2), которых сейчас нет ни одного.

**Architecture:** Компании живут в `pg-auth` (владелец данных — service-auth). `company_id` попадает в JWT рядом с `sub`, поэтому core и commission узнают владельца сделки локально, без сетевого вызова в auth. `libs/common` получает `company_id` в `Claims`, единый конверт ошибок `{code, error}` и становится полноценной линкуемой библиотекой для auth (сейчас auth берёт из неё только заголовки). Тесты — Catch2 через FetchContent, запускаются локально и в CI одной целью `make test-cpp`; ключи для тестов генерируются во время сборки, репозиторные `infra/keys` не нужны.

**Tech Stack:** C++20, Drogon 1.9 (корутины), libjwt 3.2, argon2, PostgreSQL 16, Catch2 v3.7.1 (FetchContent), CMake ≥3.20, Docker Compose, Helm.

**Spec:** `docs/superpowers/specs/2026-09-15-cbdc-hub-integration-design.md` (§3.2 изменения service-auth, §4 использование `company_id`, §9 unit C++ Catch2)

## Global Constraints

- Все команды — из корня репо `/home/legors/Documents/IVR`, ветка `feat/auth-companies` (создаётся в Task 1), базовая ветка `main`.
- Конверт ошибок всех C++-сервисов: `{"code": "<UPPER_SNAKE>", "error": "<сообщение по-русски>"}`. Коды: `INVALID_JSON`, `VALIDATION_ERROR`, `INVALID_CREDENTIALS`, `USER_EXISTS`, `UNAUTHORIZED`, `INVALID_TOKEN`, `NOT_FOUND`, `INTERNAL_ERROR`.
- JWT: `RS256`, `iss = "service-auth"`, `aud = "internal"`, claims `sub` (user_id), `company_id`, `iat`, `exp`. TTL по умолчанию 7 дней (как сейчас).
- Токен без `company_id` считается невалидным (`common::JwtVerifier::verify` → `nullopt`). Старые токены перестают работать — это ожидаемо и описывается в README.
- ИНН юрлица: ровно 10 цифр (`^[0-9]{10}$`). Пароль: не короче 6 символов (как сейчас).
- Домашняя страна, валюты и прочее из плана 01 не трогаем; service-commission в этом плане не меняется.
- Тесты C++ собираются только при `-DBUILD_TESTS=ON` (корневой workspace включает их сам). Docker-сборки сервисов тесты не собирают.
- Никаких операций с кластером и `git push`. Ветка локальная.
- Сообщение коммита заканчивается строкой `Co-Authored-By: Claude <модель> <noreply@anthropic.com>`, где модель — та, что реально пишет коммит (берётся из attribution-инструкции своего агента).

## File Structure

```
cmake/Catch2.cmake                         NEW  FetchContent Catch2 v3.7.1 (один раз на workspace)
CMakeLists.txt                             MOD  option(BUILD_TESTS), enable_testing(), include(cmake/Catch2.cmake)
infra/gen-keys.sh                          MOD  принимает каталог вывода: gen-keys.sh [outdir]

libs/testing/CMakeLists.txt                NEW  INTERFACE-библиотека testing_support
libs/testing/include/testing/token_factory.h  NEW  выпуск произвольных JWT в тестах (libjwt)

libs/common/include/common/helpers.h       MOD  jsonError(status, code, message)
libs/common/include/common/jwt_verifier.h  MOD  Claims.company_id
libs/common/src/jwt_verifier.cc            MOD  извлечение и обязательность company_id
libs/common/src/jwt_filter.cc              MOD  атрибуты user_id + company_id, коды ошибок
libs/common/CMakeLists.txt                 MOD  add_subdirectory(tests) при BUILD_TESTS
libs/common/tests/CMakeLists.txt           NEW
libs/common/tests/test_jwt_verifier.cc     NEW

services/service-auth/migrations/002_company.sql   NEW  companies + users.company_id
services/service-auth/include/jwt_issuer.h         MOD  issue(user_id, company_id, ttl)
services/service-auth/src/jwt_issuer.cc            MOD  claim company_id
services/service-auth/include/validation.h         NEW  RegisterInput + validateRegister (чистая функция)
services/service-auth/src/validation.cc            NEW
services/service-auth/src/auth/register.cc         MOD  компания по ИНН в одной транзакции
services/service-auth/src/auth/login.cc            MOD  company_id в токене, коды ошибок
services/service-auth/src/auth/me.cc               NEW  GET /api/auth/me
services/service-auth/include/auth_controller.h    MOD  метод me + аннотации для OpenAPI
services/service-auth/CMakeLists.txt               MOD  линковка common, tests при BUILD_TESTS
services/service-auth/tests/CMakeLists.txt         NEW
services/service-auth/tests/test_validation.cc     NEW
services/service-auth/tests/test_jwt_issuer.cc     NEW

Makefile                                   MOD  test-cpp, обновлённые smoke-цели, test-me
docker-compose.yml                         MOD  pg-auth наружу на 127.0.0.1:5433 (для отладки)
README.md                                  MOD  компании, /me, ломающее изменение токенов
docs/openapi.yml                           MOD  перегенерация (make openapi)
```

---

### Task 1: Каркас тестов C++ (Catch2) и тестовые ключи

**Files:**
- Create: `cmake/Catch2.cmake`, `libs/testing/CMakeLists.txt`, `libs/testing/include/testing/token_factory.h`, `libs/common/tests/CMakeLists.txt`, `libs/common/tests/test_jwt_verifier.cc`
- Modify: `CMakeLists.txt`, `infra/gen-keys.sh`, `libs/common/CMakeLists.txt`, `Makefile`

**Interfaces:**
- Consumes: существующий `common::JwtVerifier` (singleton, читает `JWT_PUBLIC_KEY_PATH` при первом обращении), `infra/gen-keys.sh`.
- Produces:
  - CMake-опция `BUILD_TESTS` (в корневом workspace — ON, в standalone-сборках сервисов — OFF), цель `Catch2::Catch2WithMain`.
  - `bash infra/gen-keys.sh <outdir>` — генерирует пару в указанный каталог (по умолчанию `infra/keys`).
  - INTERFACE-библиотека `testing_support` с `testing_support::makeToken(...)` и `testing_support::TokenSpec`.
  - `make test-cpp` — сборка и прогон всех C++-тестов.

- [ ] **Step 1: Создать ветку**

Run:
```bash
git checkout main && git status --short && git checkout -b feat/auth-companies
```
Expected: рабочее дерево чистое, ветка создана.

- [ ] **Step 2: Параметризовать `infra/gen-keys.sh`**

Заменить начало скрипта (от `cd "$(dirname "$0")/.."` до `openssl genpkey ...`) так, чтобы каталог вывода задавался аргументом:
```bash
cd "$(dirname "$0")/.."

OUT_DIR="${1:-infra/keys}"
mkdir -p "$OUT_DIR"

if [[ -f "$OUT_DIR/jwt_private.pem" && -f "$OUT_DIR/jwt_private.jwk" ]]; then
    echo "Ключи уже существуют в $OUT_DIR, ничего не делаю"
    exit 0
fi

openssl genpkey -algorithm RSA -out "$OUT_DIR/jwt_private.pem" -pkeyopt rsa_keygen_bits:2048
openssl rsa -pubout -in "$OUT_DIR/jwt_private.pem" -out "$OUT_DIR/jwt_public.pem"
```
В python-блоке заменить все литералы `infra/keys/...` на чтение каталога из переменной окружения:
- перед `python3 -c "` добавить строку `export OUT_DIR`;
- в начале python-скрипта добавить `import os` и `out = os.environ['OUT_DIR']`;
- пути внутри: `f'{out}/jwt_private.pem'`, `f'{out}/jwt_private.jwk'`, `f'{out}/jwt_public.jwk'`.

В `chmod` и `echo` в конце тоже подставить `$OUT_DIR`.

Run: `bash infra/gen-keys.sh /tmp/claude-1000/-home-legors-Documents/e9e4492d-fa15-4d6f-b2aa-e1d1470f347c/scratchpad/k1 && ls /tmp/claude-1000/-home-legors-Documents/e9e4492d-fa15-4d6f-b2aa-e1d1470f347c/scratchpad/k1`
Expected: четыре файла `jwt_private.pem/.jwk`, `jwt_public.pem/.jwk`. Повторный запуск печатает «Ключи уже существуют».
Затем проверить, что дефолт не сломан: `bash infra/gen-keys.sh` → «Ключи уже существуют в infra/keys».

- [ ] **Step 3: `cmake/Catch2.cmake`**

```cmake
# Catch2 подтягивается один раз на весь workspace: и libs/common/tests, и
# services/*/tests линкуются к одной и той же цели Catch2::Catch2WithMain.
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
```

- [ ] **Step 4: Корневой `CMakeLists.txt`**

Заменить содержимое на:
```cmake
cmake_minimum_required(VERSION 3.20)
project(ivr_workspace)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Тесты собираются только в workspace-сборке: Dockerfile каждого сервиса
# конфигурирует его CMakeLists напрямую, и там BUILD_TESTS не определён.
option(BUILD_TESTS "Build C++ unit tests" ON)

if(BUILD_TESTS)
  enable_testing()
  include(cmake/Catch2.cmake)
  add_subdirectory(libs/testing)

  # Две независимые пары ключей: A — «своя», B — чужая подпись.
  set(TEST_KEYS_A ${CMAKE_BINARY_DIR}/test-keys-a)
  set(TEST_KEYS_B ${CMAKE_BINARY_DIR}/test-keys-b)
  execute_process(COMMAND bash ${CMAKE_SOURCE_DIR}/infra/gen-keys.sh ${TEST_KEYS_A}
                  RESULT_VARIABLE keys_a_rc OUTPUT_QUIET)
  execute_process(COMMAND bash ${CMAKE_SOURCE_DIR}/infra/gen-keys.sh ${TEST_KEYS_B}
                  RESULT_VARIABLE keys_b_rc OUTPUT_QUIET)
  if(NOT keys_a_rc EQUAL 0 OR NOT keys_b_rc EQUAL 0)
    message(FATAL_ERROR "не удалось сгенерировать тестовые ключи (нужны openssl и python3 с cryptography)")
  endif()
endif()

add_subdirectory(libs/common)
add_subdirectory(services/service-auth)
add_subdirectory(services/service-core)
```

- [ ] **Step 5: `libs/testing`**

`libs/testing/CMakeLists.txt`:
```cmake
# Каталог добавляется раньше libs/common, поэтому цель PkgConfig::JWT
# может ещё не существовать — заводим её здесь, как это делает common.
if(NOT TARGET PkgConfig::JWT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)
endif()

add_library(testing_support INTERFACE)
target_include_directories(testing_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(testing_support INTERFACE PkgConfig::JWT)
```

`libs/testing/include/testing/token_factory.h`:
```cpp
#pragma once

// Выпуск произвольных JWT в тестах: нужен, чтобы проверять реакцию верификатора
// на чужую подпись, истёкший срок и отсутствующие claims.

#include <jwt.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace testing_support {

struct TokenSpec {
  std::string sub = "11111111-1111-4111-8111-111111111111";
  std::optional<std::string> company_id = "22222222-2222-4222-8222-222222222222";
  std::string iss = "service-auth";
  std::string aud = "internal";
  int ttl_seconds = 300;  // отрицательное значение => токен уже истёк
};

inline std::string makeToken(const std::string &private_jwk_path, const TokenSpec &spec = {}) {
  jwk_set_t *set = jwks_create_fromfile(private_jwk_path.c_str());
  if (!set || jwks_error(set))
    throw std::runtime_error("не удалось прочитать JWK: " + private_jwk_path);
  auto set_guard = std::unique_ptr<jwk_set_t, decltype(&jwks_free)>(set, jwks_free);

  const jwk_item_t *key = jwks_item_get(set, 0);
  if (!key || jwks_item_error(key))
    throw std::runtime_error("некорректный JWK: " + private_jwk_path);

  jwt_builder_t *builder = jwt_builder_new();
  if (!builder)
    throw std::runtime_error("jwt_builder_new failed");
  auto guard = std::unique_ptr<jwt_builder_t, decltype(&jwt_builder_free)>(builder, jwt_builder_free);

  if (jwt_builder_setkey(builder, JWT_ALG_RS256, key) != 0)
    throw std::runtime_error("jwt_builder_setkey failed");

  jwt_builder_enable_iat(builder, 1);
  jwt_builder_time_offset(builder, JWT_CLAIM_EXP, static_cast<time_t>(spec.ttl_seconds));

  const auto set_str = [&](const char *name, const std::string &value) {
    jwt_value_t jval{};
    jval.type = JWT_VALUE_STR;
    jval.name = name;
    jval.str_val = value.c_str();
    jwt_builder_claim_set(builder, &jval);
  };

  set_str("sub", spec.sub);
  set_str("iss", spec.iss);
  set_str("aud", spec.aud);
  if (spec.company_id)
    set_str("company_id", *spec.company_id);

  char *encoded = jwt_builder_generate(builder);
  if (!encoded)
    throw std::runtime_error("jwt_builder_generate failed");
  std::string result(encoded);
  std::free(encoded);
  return result;
}

}  // namespace testing_support
```

- [ ] **Step 6: Подключить тесты к `libs/common/CMakeLists.txt`**

В конец файла добавить:
```cmake
if(BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
  add_subdirectory(tests)
endif()
```

`libs/common/tests/CMakeLists.txt`:
```cmake
add_executable(common_tests test_jwt_verifier.cc)

target_link_libraries(common_tests PRIVATE
  common
  testing_support
  Catch2::Catch2WithMain
)

catch_discover_tests(common_tests
  PROPERTIES ENVIRONMENT "JWT_PUBLIC_KEY_PATH=${TEST_KEYS_A}/jwt_public.jwk"
)
target_compile_definitions(common_tests PRIVATE
  TEST_KEYS_A="${TEST_KEYS_A}"
  TEST_KEYS_B="${TEST_KEYS_B}"
)
```

- [ ] **Step 7: Написать тесты `libs/common/tests/test_jwt_verifier.cc`**

Здесь фиксируется поведение верификатора, которое уже есть. Тесты на `company_id`
добавляет Task 2 своим RED-шагом — так каждый коммит остаётся собираемым и зелёным.
```cpp
#include <catch2/catch_test_macros.hpp>

#include <common/jwt_verifier.h>
#include <testing/token_factory.h>

using testing_support::TokenSpec;
using testing_support::makeToken;

namespace {
const std::string kOwnKey = std::string(TEST_KEYS_A) + "/jwt_private.jwk";
const std::string kForeignKey = std::string(TEST_KEYS_B) + "/jwt_private.jwk";
}  // namespace

TEST_CASE("валидный токен разбирается и отдаёт sub") {
  TokenSpec spec;
  spec.sub = "user-1";

  auto claims = common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec));

  REQUIRE(claims.has_value());
  CHECK(claims->sub == "user-1");
  CHECK(claims->iss == "service-auth");
  CHECK(claims->aud == "internal");
}

TEST_CASE("чужая подпись отклоняется") {
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kForeignKey)).has_value());
}

TEST_CASE("истёкший токен отклоняется") {
  TokenSpec spec;
  spec.ttl_seconds = -60;

  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec)).has_value());
}

TEST_CASE("чужой issuer или audience отклоняются") {
  TokenSpec foreign_iss;
  foreign_iss.iss = "someone-else";
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, foreign_iss)).has_value());

  TokenSpec foreign_aud;
  foreign_aud.aud = "public";
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, foreign_aud)).has_value());
}

TEST_CASE("мусор вместо токена отклоняется") {
  CHECK_FALSE(common::JwtVerifier::instance().verify("не.токен.вовсе").has_value());
}
```

- [ ] **Step 8: Добавить цель `test-cpp` в `Makefile`**

В `.PHONY` добавить `test-cpp`. Рядом с целью `lsp` добавить:
```makefile
test-cpp:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=ON
	cmake --build build -j$(shell nproc)
	ctest --test-dir build --output-on-failure
```
(RelWithDebInfo — потому что при `Debug` CMakeLists сервисов включают ASan/UBSan, и посторонние утечки в libjwt/drogon валят прогон; санитайзеры остаются доступны через явный `-DCMAKE_BUILD_TYPE=Debug`.)

- [ ] **Step 9: Запустить тесты**

Run: `make test-cpp`
Expected: Catch2 скачивается и собирается, `common_tests` компилируется, 5/5 PASS.
Если FetchContent не может достучаться до github.com — остановиться и сообщить (тесты без сети не собрать).

- [ ] **Step 10: Commit**

```bash
git add cmake CMakeLists.txt infra/gen-keys.sh libs/testing libs/common/CMakeLists.txt libs/common/tests Makefile
git commit -m "test(cpp): Catch2 harness, generated test keys, jwt verifier specs"
```
(в конец сообщения — строка `Co-Authored-By: ...` своей модели)

---

### Task 2: `company_id` в Claims, конверт ошибок `{code, error}`

**Files:**
- Modify: `libs/common/include/common/jwt_verifier.h`, `libs/common/src/jwt_verifier.cc`, `libs/common/src/jwt_filter.cc`, `libs/common/include/common/helpers.h`
- Modify: `services/service-auth/src/auth/login.cc`, `services/service-auth/src/auth/register.cc` (только вызовы `jsonError`)
- Test: `libs/common/tests/test_jwt_verifier.cc` (из Task 1, дописывается)

**Interfaces:**
- Consumes: тестовый каркас и `testing::makeToken` (Task 1).
- Produces:
  - `common::Claims { std::string sub, iss, aud, company_id; }`
  - `common::JwtVerifier::verify` возвращает `nullopt`, если пусты `sub` или `company_id`.
  - `common::jsonError(drogon::HttpStatusCode, std::string_view code, std::string_view message)` → тело `{"code": ..., "error": ...}`. Прежняя двухаргументная версия удаляется.
  - `common::JwtFilter` кладёт в `req->attributes()` строки `user_id` и `company_id`; ошибки — `401 {"code": "UNAUTHORIZED"|"INVALID_TOKEN", "error": ...}`.

- [ ] **Step 1: Написать падающие тесты про `company_id`**

В `libs/common/tests/test_jwt_verifier.cc` добавить два теста (перед тестом про чужую подпись):
```cpp
TEST_CASE("валидный токен отдаёт company_id") {
  TokenSpec spec;
  spec.sub = "user-1";
  spec.company_id = "company-1";

  auto claims = common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec));

  REQUIRE(claims.has_value());
  CHECK(claims->company_id == "company-1");
}

TEST_CASE("токен без company_id отклоняется") {
  TokenSpec spec;
  spec.company_id = std::nullopt;

  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec)).has_value());
}
```

Run: `make test-cpp 2>&1 | tail -20`
Expected: ошибка компиляции `no member named 'company_id' in 'common::Claims'` — это RED.

- [ ] **Step 2: Расширить `Claims`**

В `libs/common/include/common/jwt_verifier.h` в структуру `Claims` добавить поле после `sub`:
```cpp
struct Claims {
    std::string sub;         // user_id
    std::string company_id;  // юрлицо, от имени которого работает пользователь
    std::string iss;
    std::string aud;
};
```

- [ ] **Step 3: Извлекать и требовать `company_id`**

В `libs/common/src/jwt_verifier.cc`, в `extract_claims`, после блока для `sub` добавить такой же блок для `company_id`:
```cpp
    jval.type = JWT_VALUE_STR;
    jval.name = "company_id";
    jval.str_val = nullptr;
    jval.error = JWT_VALUE_ERR_NONE;
    if (jwt_claim_get(jwt, &jval) == JWT_VALUE_ERR_NONE && jval.str_val)
        claims->company_id = jval.str_val;
```
В `verify()` заменить проверку `if (c.sub.empty()) return std::nullopt;` на:
```cpp
    // Токен без company_id бесполезен: все доменные данные разделены по юрлицу.
    if (c.sub.empty() || c.company_id.empty()) return std::nullopt;
```

- [ ] **Step 4: Прогнать тесты — должны пройти**

Run: `make test-cpp`
Expected: 7/7 PASS.

- [ ] **Step 5: Конверт ошибок в `libs/common/include/common/helpers.h`**

Заменить функцию целиком:
```cpp
#pragma once

#include <drogon/HttpResponse.h>
#include <string>
#include <string_view>

namespace common {

// Единый формат ошибки для всех сервисов платформы: машиночитаемый код и
// человеческое сообщение. BFF раскладывает их по экранам, не разбирая текст.
inline drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode status,
                                         std::string_view code,
                                         std::string_view message) {
  Json::Value j;
  j["code"] = std::string(code);
  j["error"] = std::string(message);
  auto r = drogon::HttpResponse::newHttpJsonResponse(j);
  r->setStatusCode(status);
  return r;
}

}  // namespace common
```

- [ ] **Step 6: Обновить `JwtFilter`**

В `libs/common/src/jwt_filter.cc`: удалить анонимный namespace с локальной функцией `unauthorized`, подключить `#include "common/helpers.h"` и заменить тело `doFilter` на:
```cpp
void JwtFilter::doFilter(const drogon::HttpRequestPtr &req,
                         drogon::FilterCallback &&fcb,
                         drogon::FilterChainCallback &&fccb) {
  auto auth = req->getHeader("Authorization");
  constexpr std::string_view prefix = "Bearer ";
  if (auth.size() < prefix.size() ||
      std::string_view(auth).substr(0, prefix.size()) != prefix) {
    fcb(jsonError(drogon::k401Unauthorized, "UNAUTHORIZED",
                  "Отсутствует токен авторизации"));
    return;
  }

  std::string token = auth.substr(prefix.size());
  auto claims = JwtVerifier::instance().verify(token);
  if (!claims) {
    fcb(jsonError(drogon::k401Unauthorized, "INVALID_TOKEN",
                  "Недействительный или истёкший токен"));
    return;
  }

  req->attributes()->insert("user_id", claims->sub);
  req->attributes()->insert("company_id", claims->company_id);
  fccb();
}
```

- [ ] **Step 7: Обновить существующие вызовы `jsonError` в auth**

Чтобы сборка не сломалась (старая двухаргументная версия удалена), заменить в `services/service-auth/src/auth/login.cc`:
- `jsonError(k400BadRequest, "invalid json")` → `jsonError(k400BadRequest, "INVALID_JSON", "Некорректный JSON в теле запроса")`
- оба `jsonError(k401Unauthorized, "invalid credentials")` → `jsonError(k401Unauthorized, "INVALID_CREDENTIALS", "Неверный логин или пароль")`
- оба `jsonError(k500InternalServerError, "internal")` → `jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса")`

и в `services/service-auth/src/auth/register.cc`:
- `jsonError(k400BadRequest, "invalid json")` → `jsonError(k400BadRequest, "INVALID_JSON", "Некорректный JSON в теле запроса")`
- `jsonError(k400BadRequest, "login required, password >= 6")` → `jsonError(k400BadRequest, "VALIDATION_ERROR", "Укажите логин и пароль не короче 6 символов")`
- `jsonError(k409Conflict, "user already exists")` → `jsonError(k409Conflict, "USER_EXISTS", "Пользователь с таким логином уже существует")`
- оба `jsonError(k500InternalServerError, "internal")` → `jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса")`

- [ ] **Step 8: Собрать всё и прогнать тесты**

Run: `make test-cpp`
Expected: сборка `common`, `service_auth`, `service_core` и тестов проходит; 7/7 PASS.

- [ ] **Step 9: Commit**

```bash
git add libs/common services/service-auth/src/auth
git commit -m "feat(common): company_id claim and {code,error} envelope"
```

---

### Task 3: Компании в базе и в токене (миграция, issuer, register, login)

Задача цельная намеренно: смена сигнатуры `JwtIssuer::issue` ломает `register.cc` и `login.cc`,
поэтому миграция, выпуск токена и оба обработчика меняются в одном заходе — иначе ветка
осталась бы несобираемой между коммитами.

**Files:**
- Create: `services/service-auth/migrations/002_company.sql`, `services/service-auth/include/validation.h`, `services/service-auth/src/validation.cc`, `services/service-auth/tests/CMakeLists.txt`, `services/service-auth/tests/test_validation.cc`, `services/service-auth/tests/test_jwt_issuer.cc`
- Modify: `services/service-auth/include/jwt_issuer.h`, `services/service-auth/src/jwt_issuer.cc`, `services/service-auth/src/auth/register.cc`, `services/service-auth/src/auth/login.cc`, `services/service-auth/include/auth_controller.h`, `services/service-auth/CMakeLists.txt`, `docker-compose.yml`

**Interfaces:**
- Consumes: `common::JwtVerifier` с `company_id` и `jsonError(status, code, message)` (Task 2), `testing_support::makeToken` (Task 1), `auth_svc::genUuid()` из `include/helpers.h`.
- Produces:
  - Схема `pg-auth`: `companies(id UUID PK, name TEXT, inn TEXT UNIQUE, created_at)`, `users.company_id UUID NOT NULL REFERENCES companies(id)`.
  - `auth_svc::JwtIssuer::issue(const std::string &user_id, const std::string &company_id, int ttl_seconds = 7 * 24 * 3600)`.
  - `auth_svc::RegisterInput { std::string login, password, name, company_name, inn; }`, `auth_svc::ValidationError { std::string code, message; }`, `std::optional<ValidationError> auth_svc::validateRegister(const RegisterInput &)`.
  - `POST /api/auth/register` принимает `{login, password, name?, company_name, inn}`, отвечает `{user_id, company_id, token}`; `POST /api/auth/login` отвечает `{user_id, company_id, token}`.
  - Цель `service_auth` линкуется с `common` (нужно для `JwtFilter` в Task 4); `pg-auth` доступен на `127.0.0.1:5433`.

- [ ] **Step 1: Миграция `services/service-auth/migrations/002_company.sql`**

```sql
-- Пользователь работает от имени юрлица: сделки, документы и уведомления
-- в service-core разделены по company_id, который приезжает в JWT.
CREATE TABLE IF NOT EXISTS companies (
    id         UUID PRIMARY KEY,
    name       TEXT        NOT NULL CHECK (length(trim(name)) > 0),
    inn        TEXT        NOT NULL UNIQUE CHECK (inn ~ '^[0-9]{10}$'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

ALTER TABLE users ADD COLUMN IF NOT EXISTS company_id UUID REFERENCES companies(id);

-- Существующие (тестовые) пользователи получают по собственной компании,
-- иначе колонку нельзя объявить NOT NULL.
WITH numbered AS (
    SELECT id,
           COALESCE(NULLIF(trim(name), ''), login) AS company_name,
           lpad(row_number() OVER (ORDER BY created_at, id)::text, 10, '0') AS inn
    FROM users
    WHERE company_id IS NULL
), created AS (
    INSERT INTO companies (id, name, inn)
    SELECT gen_random_uuid(), company_name, inn FROM numbered
    RETURNING id, inn
)
UPDATE users u
SET company_id = created.id
FROM numbered, created
WHERE u.id = numbered.id AND created.inn = numbered.inn;

ALTER TABLE users ALTER COLUMN company_id SET NOT NULL;

CREATE INDEX IF NOT EXISTS users_company_idx ON users(company_id);
```

- [ ] **Step 2: Пробросить порт pg-auth и применить миграцию**

В `docker-compose.yml` в блок `pg-auth` добавить перед `volumes:`:
```yaml
    ports:
      - "127.0.0.1:5433:5432"   # для ручной отладки и api-тестов
```
Run:
```bash
docker compose up -d pg-auth migrate-auth
docker compose logs migrate-auth | tail -5
PGPASSWORD=auth psql -h 127.0.0.1 -p 5433 -U auth -d auth -c '\d users' -c 'SELECT count(*) FROM companies;'
```
Expected: в логах `APPLY: 002_company.sql`, затем `Done. Applied ...`; в `\d users` колонка `company_id | uuid | not null`; таблица `companies` существует.
Если в базе остались пользователи без компании и `SET NOT NULL` падает — значит backfill не отработал; разобраться, а не удалять данные.

- [ ] **Step 3: Написать падающие тесты (issuer + валидация)**

`services/service-auth/tests/test_jwt_issuer.cc`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include <common/jwt_verifier.h>
#include <jwt_issuer.h>

// Выпуск и проверка используют разные API libjwt (builder против checker),
// поэтому round-trip гоняем на настоящей паре ключей.
TEST_CASE("выпущенный токен проходит верификацию и несёт company_id") {
  const auto token = auth_svc::JwtIssuer::instance().issue("user-42", "company-7");

  auto claims = common::JwtVerifier::instance().verify(token);

  REQUIRE(claims.has_value());
  CHECK(claims->sub == "user-42");
  CHECK(claims->company_id == "company-7");
}

TEST_CASE("истёкший токен не проходит верификацию") {
  const auto token = auth_svc::JwtIssuer::instance().issue("user-42", "company-7", -60);

  CHECK_FALSE(common::JwtVerifier::instance().verify(token).has_value());
}
```

`services/service-auth/tests/test_validation.cc`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include <validation.h>

using auth_svc::RegisterInput;
using auth_svc::validateRegister;

namespace {
RegisterInput valid() {
  return RegisterInput{"egor", "hunter22", "Егор", "ООО Ромашка", "7707083893"};
}
}  // namespace

TEST_CASE("корректный ввод проходит") {
  CHECK_FALSE(validateRegister(valid()).has_value());
}

TEST_CASE("логин обязателен") {
  auto input = valid();
  input.login = "   ";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->code == "VALIDATION_ERROR");
  CHECK(error->message == "Укажите логин");
}

TEST_CASE("пароль короче шести символов отклоняется") {
  auto input = valid();
  input.password = "12345";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->message == "Пароль должен быть не короче 6 символов");
}

TEST_CASE("название компании обязательно") {
  auto input = valid();
  input.company_name = "  ";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->message == "Укажите название компании");
}

TEST_CASE("ИНН — ровно 10 цифр") {
  for (const auto &bad : {"", "770708389", "77070838933", "77070838a3", "770 0838933"}) {
    auto input = valid();
    input.inn = bad;
    auto error = validateRegister(input);
    REQUIRE(error.has_value());
    CHECK(error->message == "ИНН юрлица состоит из 10 цифр");
  }
}
```

`services/service-auth/tests/CMakeLists.txt`:
```cmake
add_executable(auth_tests
  test_jwt_issuer.cc
  test_validation.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/jwt_issuer.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/validation.cc
)

target_include_directories(auth_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(auth_tests PRIVATE
  common
  testing_support
  Catch2::Catch2WithMain
)

catch_discover_tests(auth_tests
  PROPERTIES ENVIRONMENT "JWT_PUBLIC_KEY_PATH=${TEST_KEYS_A}/jwt_public.jwk;JWT_PRIVATE_KEY_PATH=${TEST_KEYS_A}/jwt_private.jwk"
)
```

В `services/service-auth/CMakeLists.txt` заменить блок `target_link_libraries(service_auth PRIVATE ...)` вместе со следующим за ним `target_include_directories(service_auth PRIVATE $<TARGET_PROPERTY:common,INTERFACE_INCLUDE_DIRECTORIES>)` на:
```cmake
target_link_libraries(service_auth PRIVATE
  common
  Drogon::Drogon
  OpenSSL::SSL OpenSSL::Crypto
  PkgConfig::ARGON2
  PkgConfig::JWT
)

if(BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
  add_subdirectory(tests)
endif()
```
(`common` — OBJECT-библиотека: её объектные файлы линкуются в бинарь, отдельный include-хак больше не нужен.)

- [ ] **Step 4: Запустить — должно упасть**

Run: `make test-cpp 2>&1 | tail -20`
Expected: ошибки компиляции — `validation.h: No such file or directory` и вызов `issue` с двумя аргументами при одном параметре.

- [ ] **Step 5: `company_id` в выпуске токена**

В `services/service-auth/include/jwt_issuer.h`:
```cpp
  std::string issue(const std::string &user_id, const std::string &company_id,
                    int ttl_seconds = 7 * 24 * 3600);
```
В `services/service-auth/src/jwt_issuer.cc` — та же сигнатура, и после блока установки claim `sub` добавить:
```cpp
  jval.type = JWT_VALUE_STR;
  jval.name = "company_id";
  jval.str_val = company_id.c_str();
  jwt_builder_claim_set(builder, &jval);
```

- [ ] **Step 6: Реализовать валидацию**

`services/service-auth/include/validation.h`:
```cpp
#pragma once

#include <optional>
#include <string>

namespace auth_svc {

struct RegisterInput {
  std::string login;
  std::string password;
  std::string name;
  std::string company_name;
  std::string inn;
};

struct ValidationError {
  std::string code;
  std::string message;
};

// Чистая проверка полей: без БД и HTTP, поэтому покрыта юнит-тестами.
std::optional<ValidationError> validateRegister(const RegisterInput &input);

}  // namespace auth_svc
```

`services/service-auth/src/validation.cc`:
```cpp
#include "validation.h"

#include <algorithm>
#include <cctype>

namespace auth_svc {

namespace {

std::string trim(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\n\r");
  if (begin == std::string::npos)
    return {};
  const auto end = value.find_last_not_of(" \t\n\r");
  return value.substr(begin, end - begin + 1);
}

bool isInn(const std::string &value) {
  return value.size() == 10 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

ValidationError error(std::string message) {
  return ValidationError{"VALIDATION_ERROR", std::move(message)};
}

}  // namespace

std::optional<ValidationError> validateRegister(const RegisterInput &input) {
  if (trim(input.login).empty())
    return error("Укажите логин");
  if (input.password.size() < 6)
    return error("Пароль должен быть не короче 6 символов");
  if (trim(input.company_name).empty())
    return error("Укажите название компании");
  if (!isInn(input.inn))
    return error("ИНН юрлица состоит из 10 цифр");
  return std::nullopt;
}

}  // namespace auth_svc
```

- [ ] **Step 7: Переписать `services/service-auth/src/auth/register.cc`**

```cpp
#include "auth_controller.h"
#include "helpers.h"
#include "jwt_issuer.h"
#include "password.h"
#include "validation.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

Task<>
AuthController::registerUser(const HttpRequestPtr req,
                             const std::function<void(const HttpResponsePtr &)> cb) {

  const auto json = req->getJsonObject();
  if (!json) {
    cb(jsonError(k400BadRequest, "INVALID_JSON", "Некорректный JSON в теле запроса"));
    co_return;
  }

  const RegisterInput input{
      (*json)["login"].asString(),
      (*json)["password"].asString(),
      json->get("name", "").asString(),
      (*json)["company_name"].asString(),
      (*json)["inn"].asString(),
  };

  if (const auto invalid = validateRegister(input)) {
    cb(jsonError(k400BadRequest, invalid->code, invalid->message));
    co_return;
  }

  const auto db = app().getDbClient();

  try {
    auto tx = co_await db->newTransactionCoro();

    const auto exists =
        co_await tx->execSqlCoro("SELECT 1 FROM users WHERE login = $1", input.login);
    if (exists.size() > 0) {
      cb(jsonError(k409Conflict, "USER_EXISTS",
                   "Пользователь с таким логином уже существует"));
      co_return;
    }

    // Компания заводится один раз на ИНН: второй сотрудник того же юрлица
    // присоединяется к существующей записи, а не создаёт дубль.
    auto company = co_await tx->execSqlCoro(
        "INSERT INTO companies (id, name, inn) VALUES ($1, $2, $3) "
        "ON CONFLICT (inn) DO NOTHING RETURNING id",
        genUuid(), input.company_name, input.inn);
    if (company.size() == 0) {
      company = co_await tx->execSqlCoro("SELECT id FROM companies WHERE inn = $1",
                                         input.inn);
    }
    const auto company_id = company[0]["id"].as<std::string>();

    const auto user_id = genUuid();
    co_await tx->execSqlCoro(
        "INSERT INTO users (id, login, password_hash, name, company_id) "
        "VALUES ($1, $2, $3, $4, $5)",
        user_id, input.login, hashPassword(input.password), input.name, company_id);

    const auto token = JwtIssuer::instance().issue(user_id, company_id);

    Json::Value out;
    out["user_id"] = user_id;
    out["company_id"] = company_id;
    out["token"] = token;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "register db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "register error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
```

- [ ] **Step 8: Обновить `services/service-auth/src/auth/login.cc`**

- В SQL заменить `"SELECT id, password_hash FROM users WHERE login = $1"` на `"SELECT id, password_hash, company_id FROM users WHERE login = $1"`.
- После строки `std::string pw_hash = rows[0]["password_hash"].as<std::string>();` добавить `std::string company_id = rows[0]["company_id"].as<std::string>();`.
- Заменить `auto token = JwtIssuer::instance().issue(user_id);` на `auto token = JwtIssuer::instance().issue(user_id, company_id);`.
- В формировании ответа перед `out["token"]` добавить `out["company_id"] = company_id;`.

- [ ] **Step 9: Обновить аннотации в `services/service-auth/include/auth_controller.h`**

Для `registerUser`:
```cpp
  // @body   {"login": "string", "password": "string", "name?": "string", "company_name": "string", "inn": "string"}
  // @200    {"user_id": "string", "company_id": "string", "token": "string"}
```
Для `login`:
```cpp
  // @200    {"user_id": "string", "company_id": "string", "token": "string"}
```

- [ ] **Step 10: Прогнать тесты**

Run: `make test-cpp`
Expected: 14/14 PASS (7 в `common_tests`, 7 в `auth_tests`).

- [ ] **Step 11: Проверить на живом сервисе**

```bash
docker compose up -d --build service-auth traefik pg-auth migrate-auth
docker compose ps service-auth
curl -s -X POST http://localhost/api/auth/register -H 'Content-Type: application/json' \
  -d '{"login":"egor","password":"hunter22","name":"Егор","company_name":"ООО Ромашка","inn":"7707083893"}' | jq
curl -s -X POST http://localhost/api/auth/register -H 'Content-Type: application/json' \
  -d '{"login":"maria","password":"hunter22","name":"Мария","company_name":"ООО Ромашка","inn":"7707083893"}' | jq
curl -s -X POST http://localhost/api/auth/register -H 'Content-Type: application/json' \
  -d '{"login":"bad","password":"hunter22","company_name":"ООО Ромашка","inn":"770"}' | jq
curl -s -X POST http://localhost/api/auth/login -H 'Content-Type: application/json' \
  -d '{"login":"egor","password":"hunter22"}' | jq
```
Expected: у `egor` и `maria` совпадает `company_id`; третий запрос — `400 {"code":"VALIDATION_ERROR","error":"ИНН юрлица состоит из 10 цифр"}`; логин возвращает тот же `company_id`.
Если логин `egor` уже занят с прошлых прогонов — использовать другие логины и отметить это в отчёте. Если порт 8081 занят посторонним процессом и `traefik` не поднимается — работать без него, обращаясь к контейнеру напрямую (`docker compose exec`), и отметить это в отчёте.

- [ ] **Step 12: Commit**

```bash
git add services/service-auth docker-compose.yml
git commit -m "feat(auth): companies by INN, company_id in tokens and responses"
```
(в конец сообщения — строка `Co-Authored-By: ...` своей модели)

---

### Task 4: `GET /api/auth/me`, документация и smoke-цели

**Files:**
- Create: `services/service-auth/src/auth/me.cc`
- Modify: `services/service-auth/include/auth_controller.h`, `Makefile`, `README.md`, `docs/openapi.yml` (перегенерация)

**Interfaces:**
- Consumes: `common::JwtFilter` с атрибутами `user_id`/`company_id` (Task 2), таблицы `users`/`companies` (Task 3).
- Produces: `GET /api/auth/me` (требует `Authorization: Bearer <token>`) → `{"user_id", "login", "name", "company": {"id", "name", "inn"}}`; цели `make test-register`, `make test-login`, `make test-me`.

- [ ] **Step 1: Объявить метод в `services/service-auth/include/auth_controller.h`**

После блока `login` в `METHOD_LIST_BEGIN/END` добавить:
```cpp
  // @GET /api/auth/me
  // @summary Профиль текущего пользователя и его компания
  // @header Authorization: Bearer <token>
  // @200    {"user_id": "string", "login": "string", "name": "string"}
  // @401    {"code": "string", "error": "string"}
  ADD_METHOD_TO(AuthController::me, "/api/auth/me", drogon::Get, "common::JwtFilter");
```
и к списку объявлений методов класса:
```cpp
  static drogon::Task<> me(drogon::HttpRequestPtr req,
                           std::function<void(const drogon::HttpResponsePtr &)> cb);
```

- [ ] **Step 2: Реализовать `services/service-auth/src/auth/me.cc`**

```cpp
#include "auth_controller.h"
#include "helpers.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

// Кто я и от имени какого юрлица работаю. Источник данных для шапки кабинета:
// BFF зовёт эту ручку после логина и при восстановлении сессии из cookie.
Task<> AuthController::me(HttpRequestPtr req,
                          std::function<void(const HttpResponsePtr &)> cb) {

  const auto user_id = req->attributes()->get<std::string>("user_id");
  const auto db = app().getDbClient();

  try {
    const auto rows = co_await db->execSqlCoro(
        "SELECT u.login, u.name, c.id AS company_id, c.name AS company_name, c.inn "
        "FROM users u JOIN companies c ON c.id = u.company_id "
        "WHERE u.id = $1",
        user_id);

    if (rows.size() == 0) {
      // Токен валиден, но пользователя уже нет — например, базу пересоздали.
      cb(jsonError(k404NotFound, "NOT_FOUND", "Пользователь не найден"));
      co_return;
    }

    Json::Value company;
    company["id"] = rows[0]["company_id"].as<std::string>();
    company["name"] = rows[0]["company_name"].as<std::string>();
    company["inn"] = rows[0]["inn"].as<std::string>();

    Json::Value out;
    out["user_id"] = user_id;
    out["login"] = rows[0]["login"].as<std::string>();
    out["name"] = rows[0]["name"].as<std::string>();
    out["company"] = company;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "me db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
```

- [ ] **Step 3: Собрать и проверить ручку**

```bash
make test-cpp
docker compose up -d --build service-auth
TOKEN=$(curl -s -X POST http://localhost/api/auth/login -H 'Content-Type: application/json' \
  -d '{"login":"egor","password":"hunter22"}' | jq -r .token)
curl -s http://localhost/api/auth/me -H "Authorization: Bearer $TOKEN" | jq
curl -s -o /dev/null -w '%{http_code}\n' http://localhost/api/auth/me
curl -s http://localhost/api/auth/me -H "Authorization: Bearer мусор" | jq
```
Expected: 14/14 PASS; профиль с блоком `company` (`inn` = `7707083893`); без заголовка — `401`; с мусором — `{"code":"INVALID_TOKEN", ...}`.
(Логин подставить тот, что реально заведён в Task 3.)

- [ ] **Step 4: Обновить smoke-цели в `Makefile`**

В `.PHONY` добавить `test-me`. Заменить цели `test-register`, `test-login` и добавить `test-me`:
```makefile
test-register:
	curl -s -X POST http://localhost/api/auth/register \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter22","name":"Егор","company_name":"ООО Ромашка","inn":"7707083893"}' | jq

test-login:
	curl -s -X POST http://localhost/api/auth/login \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter22"}' | jq

test-me:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
	curl -s http://localhost/api/auth/me -H "Authorization: Bearer $$TOKEN" | jq
```
В цели `k3s-test-auth` в теле регистрации добавить `"company_name":"ООО Тест","inn":"7710137066"`.

- [ ] **Step 5: Перегенерировать OpenAPI**

Run: `make openapi && grep -n "api/auth/me" docs/openapi.yml`
Expected: `FastAPI endpoints: 2`, в списке C++-ручек появился `/api/auth/me`, grep находит путь.

- [ ] **Step 6: Обновить `README.md`**

- В быстрый старт после `make test-login` добавить:
  ```
  export TOKEN=<твой токен>
  make test-me
  ```
- В раздел «Принципы» добавить пункт:
  ```
  5. **Пользователь = сотрудник юрлица** — `company_id` едет в JWT, поэтому core и
     commission разделяют данные по компании, не обращаясь в auth по сети.
  ```
- В блоке «Что НЕ сделано» удалить строки про заглушку `service-auth/me` и про отсутствие тестов Catch2.
- Перед блоком «Что НЕ сделано» добавить раздел:
  ```markdown
  ## Ломающее изменение: компании (план 02)

  Регистрация требует `company_name` и `inn` (10 цифр). Пользователи с одинаковым ИНН
  попадают в одну компанию. В JWT добавлен claim `company_id`, и токены **без него
  считаются невалидными** — выданные раньше токены нужно перевыпустить (повторный логин).
  Ошибки C++-сервисов отдаются в формате `{"code": "...", "error": "..."}`.

  Тесты C++ (Catch2, собираются только в workspace-сборке):

      make test-cpp
  ```

- [ ] **Step 7: Финальная проверка**

Run: `make test-cpp && git status --short`
Expected: 14/14 PASS; в выводе `git status` только файлы, которые уйдут в коммит Step 8.

- [ ] **Step 8: Commit**

```bash
git add services/service-auth Makefile README.md docs/openapi.yml
git commit -m "feat(auth): GET /api/auth/me, smoke targets and docs"
```
