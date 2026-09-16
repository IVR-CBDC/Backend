#include "core_controller.h"

#include <algorithm>
#include <common/helpers.h>
#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

namespace {

// Shared with deals.cc's listDeals limit parsing (kept duplicated per the
// brief's file scope for this task — notifications.cc only).
bool parseLimit(const std::string &raw, int &out) {
  if (raw.empty()) {
    out = 50;
    return true;
  }
  try {
    size_t consumed = 0;
    out = std::stoi(raw, &consumed);
    return consumed == raw.size();
  } catch (...) {
    return false;
  }
}

}  // namespace

// Список уведомлений компании, сортировка created_at DESC, лимит 1..100.
Task<> CoreController::listNotifications(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> cb,
                                         std::string limit) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  int parsed = 50;
  if (!parseLimit(limit, parsed)) {
    cb(jsonError(k400BadRequest, "VALIDATION_ERROR", "limit должен быть числом"));
    co_return;
  }
  int clamped = std::clamp(parsed, 1, 100);

  try {
    auto db = app().getDbClient();

    // See repository.cc's list(): LIMIT needs an explicit ::int cast, else
    // Postgres infers int8 and rejects Drogon's 4-byte int bind.
    auto rows = co_await db->execSqlCoro(
        "SELECT id::text AS id, deal_id::text AS deal_id, severity, message, "
        "       (read_at IS NOT NULL) AS read, "
        "       to_char(created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created_at "
        "FROM notifications WHERE company_id = $1::uuid ORDER BY created_at DESC LIMIT $2::int",
        company_id, clamped);

    auto unreadRows = co_await db->execSqlCoro(
        "SELECT count(*) AS n FROM notifications WHERE company_id = $1::uuid AND read_at IS NULL", company_id);

    Json::Value items(Json::arrayValue);
    for (const auto &row : rows) {
      Json::Value j;
      j["id"] = row["id"].as<std::string>();
      j["deal_id"] = row["deal_id"].as<std::string>();
      j["severity"] = row["severity"].as<std::string>();
      j["message"] = row["message"].as<std::string>();
      j["read"] = row["read"].as<bool>();
      j["created_at"] = row["created_at"].as<std::string>();
      items.append(j);
    }

    Json::Value out;
    out["items"] = items;
    out["unread"] = unreadRows[0]["n"].as<int>();
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "listNotifications db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "listNotifications error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}

// Отмечает уведомление прочитанным. Ноль обновлённых строк (не найдено,
// чужая компания, либо уже прочитано) — всё одинаково 404: ручка не
// раскрывает, какая из причин сработала.
Task<> CoreController::markNotificationRead(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> cb,
                                            std::string id) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  try {
    auto db = app().getDbClient();
    auto result = co_await db->execSqlCoro(
        "UPDATE notifications SET read_at = now() WHERE id = $1::uuid AND company_id = $2::uuid AND read_at IS NULL",
        id, company_id);

    if (result.affectedRows() == 0) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Уведомление не найдено"));
      co_return;
    }

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k204NoContent);
    cb(resp);

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "markNotificationRead db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "markNotificationRead error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
