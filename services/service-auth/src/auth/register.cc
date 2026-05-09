#include "auth_controller.h"
#include "helpers.h"
#include "jwt_issuer.h"
#include "password.h"

#include <drogon/drogon.h>

using namespace auth_svc;
using namespace drogon;

Task<>
AuthController::registerUser(const HttpRequestPtr req,
                             const std::function<void(const HttpResponsePtr &)> cb) {

  const auto json = req->getJsonObject();
  if (!json) {
    cb(jsonError(k400BadRequest, "invalid json"));
    co_return;
  }

  const std::string login = (*json)["login"].asString();
  const std::string password = (*json)["password"].asString();
  const std::string name = json->get("name", "").asString();

  if (login.empty() || password.size() < 6) {
    cb(jsonError(k400BadRequest, "login required, password >= 6"));
    co_return;
  }

  const auto db = app().getDbClient();

  try {
    const auto exists =
        co_await db->execSqlCoro("SELECT 1 FROM users WHERE login = $1", login);
    if (exists.size() > 0) {
      cb(jsonError(k409Conflict, "user already exists"));
      co_return;
    }

    auto user_id = genUuid();
    auto hash = hashPassword(password);

    co_await db->execSqlCoro(
        "INSERT INTO users (id, login, password_hash, name) "
        "VALUES ($1, $2, $3, $4)",
        user_id, login, hash, name);

    const auto token = JwtIssuer::instance().issue(user_id);

    Json::Value out;
    out["user_id"] = user_id;
    out["token"] = token;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "register db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "internal"));
  } catch (const std::exception &e) {
    LOG_ERROR << "register error: " << e.what();
    cb(jsonError(k500InternalServerError, "internal"));
  }
}
