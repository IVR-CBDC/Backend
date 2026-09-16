#pragma once
#include <drogon/HttpController.h>

namespace core_svc {

class CoreController : public drogon::HttpController<CoreController> {
public:
  METHOD_LIST_BEGIN
  // @GET /health
  // @summary Health check service-core
  // @200    {"ok": "boolean", "service": "string", "version": "string", "postgres_ok": "boolean", "redis_ok": "boolean"}
  ADD_METHOD_TO(CoreController::health, "/health", drogon::Get);

  // @GET /api/core/deals
  // @summary Список сделок компании
  // @header Authorization: Bearer <token>
  // @200    {"items": "array", "count": "integer"}
  // @401    {"code": "string", "error": "string"}
  ADD_METHOD_TO(CoreController::listDeals, "/api/core/deals?limit={limit}", drogon::Get,
                "common::JwtFilter");

  // @GET /api/core/deals/{id}
  // @summary Сделка целиком: документы и таймлайн
  // @header Authorization: Bearer <token>
  // @200    {"deal": "object"}
  // @404    {"code": "string", "error": "string"}
  ADD_METHOD_TO(CoreController::getDeal, "/api/core/deals/{id}", drogon::Get,
                "common::JwtFilter");
  METHOD_LIST_END

  drogon::Task<>
  health(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);
  static drogon::Task<> listDeals(drogon::HttpRequestPtr req,
                                  std::function<void(const drogon::HttpResponsePtr &)> cb,
                                  std::string limit);
  static drogon::Task<> getDeal(drogon::HttpRequestPtr req,
                                std::function<void(const drogon::HttpResponsePtr &)> cb,
                                std::string id);
};

} // namespace core_svc
