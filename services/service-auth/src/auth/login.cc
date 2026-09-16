#include "auth_controller.h"
#include "helpers.h"
#include "jwt_issuer.h"
#include "password.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

Task<> AuthController::login(HttpRequestPtr req,
                             std::function<void(const HttpResponsePtr &)> cb) {

  auto json = req->getJsonObject();
  if (!json) {
    cb(jsonError(k400BadRequest, "INVALID_JSON", "Некорректный JSON в теле запроса"));
    co_return;
  }

  std::string login = (*json)["login"].asString();
  std::string password = (*json)["password"].asString();

  auto db = app().getDbClient();

  try {
    auto rows = co_await db->execSqlCoro(
        "SELECT id, password_hash, company_id FROM users WHERE login = $1", login);
    if (rows.size() == 0) {
      cb(jsonError(k401Unauthorized, "INVALID_CREDENTIALS", "Неверный логин или пароль"));
      co_return;
    }

    std::string user_id = rows[0]["id"].as<std::string>();
    std::string pw_hash = rows[0]["password_hash"].as<std::string>();
    std::string company_id = rows[0]["company_id"].as<std::string>();

    if (!verifyPassword(password, pw_hash)) {
      cb(jsonError(k401Unauthorized, "INVALID_CREDENTIALS", "Неверный логин или пароль"));
      co_return;
    }

    auto token = JwtIssuer::instance().issue(user_id, company_id);
    Json::Value out;
    out["user_id"] = user_id;
    out["company_id"] = company_id;
    out["token"] = token;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "login db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "login error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
