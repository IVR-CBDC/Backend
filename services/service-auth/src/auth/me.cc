#include "auth_controller.h"
#include "helpers.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

// Кто я и от имени какого юрлица работаю. Источник данных для шапки кабинета:
// BFF зовёт эту ручку после логина и при восстановлении сессии из cookie.
Task<> AuthController::me(HttpRequestPtr req,
                          std::function<void(const HttpResponsePtr &)> cb) {

  const auto user_id = req->attributes()->get<std::string>("user_id");
  const auto db = app().getDbClient();

  try {
    const auto rows = co_await db->execSqlCoro(
        "SELECT u.login, u.name, c.id AS company_id, c.name AS company_name, c.inn "
        "FROM users u JOIN companies c ON c.id = u.company_id "
        "WHERE u.id = $1",
        user_id);

    if (rows.size() == 0) {
      // Токен валиден, но пользователя уже нет — например, базу пересоздали.
      cb(jsonError(k404NotFound, "NOT_FOUND", "Пользователь не найден"));
      co_return;
    }

    Json::Value company;
    company["id"] = rows[0]["company_id"].as<std::string>();
    company["name"] = rows[0]["company_name"].as<std::string>();
    company["inn"] = rows[0]["inn"].as<std::string>();

    Json::Value out;
    out["user_id"] = user_id;
    out["login"] = rows[0]["login"].as<std::string>();
    out["name"] = rows[0]["name"].as<std::string>();
    out["company"] = company;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "me db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
