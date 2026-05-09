#pragma once

#include <drogon/HttpController.h>

namespace auth_svc {

class AuthController : public drogon::HttpController<AuthController> {
public:
  METHOD_LIST_BEGIN
  // @POST /api/auth/register
  // @summary Регистрация нового пользователя
  // @body   {"login": "string", "password": "string", "name?": "string"}
  // @200    {"user_id": "string", "token": "string"}
  // @400    {"error": "string"}
  // @409    {"error": "string"}
  ADD_METHOD_TO(AuthController::registerUser, "/api/auth/register",
                drogon::Post);

  // @POST /api/auth/login
  // @summary Авторизация, возвращает JWT
  // @body   {"login": "string", "password": "string"}
  // @200    {"user_id": "string", "token": "string"}
  // @400    {"error": "string"}
  // @401    {"error": "string"}
  ADD_METHOD_TO(AuthController::login, "/api/auth/login", drogon::Post);


  // @GET /health
  // @summary Health check service-auth
  // @200    {"ok": "boolean", "service": "string", "version": "string",
  // "postgres_ok": "boolean"}
  ADD_METHOD_TO(AuthController::health, "/health", drogon::Get);
  ADD_METHOD_TO(AuthController::publicKey, "/api/auth/.well-known/jwks.json",
                drogon::Get);
  METHOD_LIST_END

  static drogon::Task<>
  registerUser(drogon::HttpRequestPtr req,
               std::function<void(const drogon::HttpResponsePtr &)> cb);

  static drogon::Task<> login(drogon::HttpRequestPtr req,
                       std::function<void(const drogon::HttpResponsePtr &)> cb);


  static drogon::Task<>
  health(drogon::HttpRequestPtr req,
         std::function<void(const drogon::HttpResponsePtr &)> cb);

  static void publicKey(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb);
};

} // namespace auth_svc
