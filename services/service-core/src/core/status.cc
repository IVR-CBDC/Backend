#include "core_controller.h"

#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;

Task<> CoreController::status(HttpRequestPtr req,
                              std::function<void(const HttpResponsePtr &)> cb) {
  auto user_id = req->attributes()->get<std::string>("user_id");
  Json::Value out;
  out["user_id"] = user_id;
  out["status"] = "alive";
  out["uptime_sec"] = (Json::Int64)trantor::Date::now().secondsSinceEpoch();
  cb(HttpResponse::newHttpJsonResponse(out));
  co_return;
}
