#include "auth_controller.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

Task<> AuthController::health(HttpRequestPtr,
                              std::function<void(const HttpResponsePtr &)> cb) {

  Json::Value out;
  out["ok"] = true;
  out["service"] = "service-auth";
  out["version"] = "0.1.0";

  auto db = app().getDbClient();
  try {
    co_await db->execSqlCoro("SELECT 1");
    out["postgres_ok"] = true;
  } catch (...) {
    out["postgres_ok"] = false;
    out["ok"] = false;
  }

  cb(HttpResponse::newHttpJsonResponse(out));
}
