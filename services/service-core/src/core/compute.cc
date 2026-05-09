#include "core_controller.h"

#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;

Task<>
CoreController::compute(HttpRequestPtr req,
                        std::function<void(const HttpResponsePtr &)> cb) {
  auto user_id = req->attributes()->get<std::string>("user_id");

  auto json = req->getJsonObject();
  int n = json && (*json).isMember("n") ? (*json)["n"].asInt() : 10;

  long long sum = 0;
  for (int i = 1; i <= n; ++i)
    sum += i;

  Json::Value out;
  out["user_id"] = user_id;
  out["input_n"] = n;
  out["result"] = (Json::Int64)sum;
  cb(HttpResponse::newHttpJsonResponse(out));
  co_return;
}
