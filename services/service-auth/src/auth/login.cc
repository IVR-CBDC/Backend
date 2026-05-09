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
    cb(jsonError(k400BadRequest, "invalid json"));
    co_return;
  }

  std::string login = (*json)["login"].asString();
  std::string password = (*json)["password"].asString();

  auto db = app().getDbClient();

  try {
    auto rows = co_await db->execSqlCoro(
        "SELECT id, password_hash FROM users WHERE login = $1", login);
    if (rows.size() == 0) {
      cb(jsonError(k401Unauthorized, "invalid credentials"));
      co_return;
    }

    std::string user_id = rows[0]["id"].as<std::string>();
    std::string pw_hash = rows[0]["password_hash"].as<std::string>();

    if (!verifyPassword(password, pw_hash)) {
      cb(jsonError(k401Unauthorized, "invalid credentials"));
      co_return;
    }

    auto token = JwtIssuer::instance().issue(user_id);
    Json::Value out;
    out["user_id"] = user_id;
    out["token"] = token;
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "login db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "internal"));
  } catch (const std::exception &e) {
    LOG_ERROR << "login error: " << e.what();
    cb(jsonError(k500InternalServerError, "internal"));
  }
}
