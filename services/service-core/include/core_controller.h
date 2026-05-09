#pragma once
#include <drogon/HttpController.h>

namespace core_svc {

class CoreController : public drogon::HttpController<CoreController> {
public:
  METHOD_LIST_BEGIN
  // @GET /health
  // @summary Health check service-core
  // @200    {"ok": "boolean", "service": "string", "version": "string", "postgres_ok": "boolean"}
  ADD_METHOD_TO(CoreController::health, "/health", drogon::Get);

  // @POST /api/core/compute
  // @summary Вычисляет сумму 1..n (пример защищённой ручки)
  // @header Authorization: Bearer <token>
  // @body   {"n?": "integer"}
  // @200    {"user_id": "string", "input_n": "integer", "result": "integer"}
  // @401    {"error": "string"}
  ADD_METHOD_TO(CoreController::compute, "/api/core/compute", drogon::Post,
                "common::JwtFilter");

  // @GET /api/core/status
  // @summary Статус сервиса (защищённый)
  // @header Authorization: Bearer <token>
  // @200    {"user_id": "string", "status": "string", "uptime_sec": "integer"}
  // @401    {"error": "string"}
  ADD_METHOD_TO(CoreController::status, "/api/core/status", drogon::Get,
                "common::JwtFilter");
  METHOD_LIST_END

  drogon::Task<>
  health(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);
  drogon::Task<>
  compute(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> cb);
  drogon::Task<>
  status(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);
};

} // namespace core_svc
