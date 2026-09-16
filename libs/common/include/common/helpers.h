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
