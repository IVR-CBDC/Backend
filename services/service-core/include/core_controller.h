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

  // @POST /api/core/deals
  // @summary Создание сделки
  // @header Authorization: Bearer <token>
  // @body   {"counterparty_country": "string", "counterparty_name": "string", "operation_type": "string", "amount": "number", "currency": "string"}
  // @201    {"deal": "object"}
  // @400    {"code": "string", "error": "string"}
  ADD_METHOD_TO(CoreController::createDeal, "/api/core/deals", drogon::Post, "common::JwtFilter");

  // @POST /api/core/deals/{id}/scenario
  // @summary Подтверждение сценария расчёта (комиссия пересчитывается на сервере)
  // @header Authorization: Bearer <token>
  // @body   {"scenario": "string", "version": "integer"}
  // @200    {"deal": "object"}
  // @409    {"code": "string", "error": "string"}
  ADD_METHOD_TO(CoreController::chooseScenario, "/api/core/deals/{id}/scenario", drogon::Post,
                "common::JwtFilter");

  // @POST /api/core/deals/{dealId}/documents/{docId}/submit
  // @summary Подача документа на проверку
  // @header Authorization: Bearer <token>
  // @body   {"version": "integer"}
  // @200    {"deal": "object"}
  // @409    {"code": "string", "error": "string"}
  ADD_METHOD_TO(CoreController::submitDocument,
                "/api/core/deals/{dealId}/documents/{docId}/submit", drogon::Post,
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
  static drogon::Task<> createDeal(drogon::HttpRequestPtr req,
                                   std::function<void(const drogon::HttpResponsePtr &)> cb);
  static drogon::Task<> chooseScenario(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> cb,
                                       std::string id);
  static drogon::Task<> submitDocument(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> cb,
                                       std::string dealId, std::string docId);

  // Manual emulator tick: not part of METHOD_LIST_BEGIN/ADD_METHOD_TO above
  // — main.cc registers it directly via app().registerHandler(), and only
  // when EMULATOR_MANUAL=true, so the route doesn't exist at all otherwise.
  // Internal-only (no JWT, not exposed through Traefik).
  static drogon::Task<> tickEmulator(drogon::HttpRequestPtr req,
                                     std::function<void(const drogon::HttpResponsePtr &)> cb);
};

} // namespace core_svc
