#pragma once

#include <drogon/HttpResponse.h>
#include <string>

namespace common {

inline drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode code,
                                         const std::string &msg) {
  Json::Value j;
  j["error"] = msg;
  auto r = drogon::HttpResponse::newHttpJsonResponse(j);
  r->setStatusCode(code);
  return r;
}

}  // namespace common
