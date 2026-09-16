#include "core_controller.h"

#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;

Task<> CoreController::health(HttpRequestPtr,
                              std::function<void(const HttpResponsePtr &)> cb) {
  Json::Value j;
  j["ok"] = true;
  j["service"] = "service-core";
  j["version"] = "0.1.0";

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
