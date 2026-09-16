#include "auth_controller.h"
#include "helpers.h"
#include "jwt_issuer.h"
#include "password.h"
#include "validation.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

Task<>
AuthController::registerUser(const HttpRequestPtr req,
                             const std::function<void(const HttpResponsePtr &)> cb) {

  const auto json = req->getJsonObject();
  if (!json) {
    cb(jsonError(k400BadRequest, "INVALID_JSON", "Некорректный JSON в теле запроса"));
    co_return;
  }

  const RegisterInput input{
      (*json)["login"].asString(),
      (*json)["password"].asString(),
      json->get("name", "").asString(),
      (*json)["company_name"].asString(),
      (*json)["inn"].asString(),
  };

  if (const auto invalid = validateRegister(input)) {
    cb(jsonError(k400BadRequest, invalid->code, invalid->message));
    co_return;
  }

  const auto db = app().getDbClient();

  try {
    auto tx = co_await db->newTransactionCoro();

    const auto exists =
        co_await tx->execSqlCoro("SELECT 1 FROM users WHERE login = $1", input.login);
    if (exists.size() > 0) {
      cb(jsonError(k409Conflict, "USER_EXISTS",
                   "Пользователь с таким логином уже существует"));
      co_return;
    }

    // Компания заводится один раз на ИНН: второй сотрудник того же юрлица
    // присоединяется к существующей записи, а не создаёт дубль.
    auto company = co_await tx->execSqlCoro(
        "INSERT INTO companies (id, name, inn) VALUES ($1, $2, $3) "
        "ON CONFLICT (inn) DO NOTHING RETURNING id",
        genUuid(), input.company_name, input.inn);
    if (company.size() == 0) {
      company = co_await tx->execSqlCoro("SELECT id FROM companies WHERE inn = $1",
                                         input.inn);
    }
    const auto company_id = company[0]["id"].as<std::string>();

    const auto user_id = genUuid();
    co_await tx->execSqlCoro(
        "INSERT INTO users (id, login, password_hash, name, company_id) "
        "VALUES ($1, $2, $3, $4, $5)",
        user_id, input.login, hashPassword(input.password), input.name, company_id);

    const auto token = JwtIssuer::instance().issue(user_id, company_id);

    Json::Value out;
    out["user_id"] = user_id;
    out["company_id"] = company_id;
    out["token"] = token;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "register db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "register error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
