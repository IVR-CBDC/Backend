#include "core_controller.h"

#include "emulator.h"

#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;

Task<> CoreController::health(HttpRequestPtr,
                              std::function<void(const HttpResponsePtr &)> cb) {
  Json::Value j;
  j["ok"] = true;
  j["service"] = "service-core";
  j["version"] = "0.1.0";

  // Режим эмулятора в ответе /health (план 08, Task 4). НЕ часть
  // ok/health-семантики: оба режима — штатные, поэтому поля ниже никогда не
  // опускают `ok` в false. Это диагностика конфигурации, а не здоровья.
  //
  // Зачем вообще: `POST /internal/emulator/tick` регистрируется ТОЛЬКО при
  // EMULATOR_MANUAL=true (main.cc), и при неверном режиме её единственный
  // симптом — 404 посреди e2e-теста, внешне неотличимый от опечатки в пути
  // или отвалившегося роутинга. За план 07 на это наступили дважды. Теперь
  // режим виден снаружи до первого теста: globalSetup Playwright читает
  // именно это поле и падает одной понятной строкой (Frontend-репозиторий,
  // e2e/global-setup.ts), а `make e2e-stand-up` печатает, где посмотреть.
  //
  // Значения читаются из тех же функций, что и main.cc при старте, поэтому
  // разойтись с реальным поведением процесса они не могут — это не копия
  // флага, а тот же источник.
  j["emulator_manual"] = core_svc::emulatorManual();
  j["emulator_enabled"] = core_svc::emulatorEnabled();

  auto db = app().getDbClient();
  try {
    co_await db->execSqlCoro("SELECT 1");
    j["postgres_ok"] = true;
  } catch (...) {
    j["postgres_ok"] = false;
    j["ok"] = false;
  }

  // getRedisClient() returns a null RedisClientPtr (not a throw) for a
  // client name that isn't in config.json's redis_clients — which is
  // exactly what happens if that section is ever missing (see F1: it used
  // to be, in k3s). Dereferencing a null RedisClientPtr is a segfault, not
  // an exception, so the try/catch below can't save us from it — it must be
  // guarded before ever calling into it.
  auto redis = app().getRedisClient();
  if (!redis) {
    j["redis_ok"] = false;
    j["ok"] = false;
  } else {
    try {
      co_await redis->execCommandCoro("ping");
      j["redis_ok"] = true;
    } catch (...) {
      j["redis_ok"] = false;
      j["ok"] = false;
    }
  }

  cb(HttpResponse::newHttpJsonResponse(j));
}
