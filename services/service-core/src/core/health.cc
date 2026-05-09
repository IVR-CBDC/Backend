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
  cb(HttpResponse::newHttpJsonResponse(j));
}
