#include "core_controller.h"
#include "repository.h"

#include <common/helpers.h>
#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

// Список сделок компании: `limit` из query, зажимается в 1..100 в репозитории.
Task<> CoreController::listDeals(HttpRequestPtr req,
                                 std::function<void(const HttpResponsePtr &)> cb,
                                 std::string limit) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  int parsed = 50;
  if (!limit.empty()) {
    try {
      size_t consumed = 0;
      parsed = std::stoi(limit, &consumed);
      if (consumed != limit.size()) throw std::invalid_argument("trailing garbage");
    } catch (...) {
      cb(jsonError(k400BadRequest, "VALIDATION_ERROR", "limit должен быть числом"));
      co_return;
    }
  }

  try {
    auto deals = co_await DealRepository::list(company_id, parsed);

    Json::Value items(Json::arrayValue);
    for (const auto &deal : deals) items.append(dealSummaryJson(deal));

    Json::Value out;
    out["items"] = items;
    out["count"] = static_cast<Json::UInt64>(deals.size());
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "listDeals db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "listDeals error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}

// Сделка целиком: документы и таймлайн. company_id из JWT ограничивает
// доступ так, что чужая сделка выглядит как отсутствующая (404), а не 403.
Task<> CoreController::getDeal(HttpRequestPtr req,
                               std::function<void(const HttpResponsePtr &)> cb,
                               std::string id) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  try {
    auto detail = co_await DealRepository::find(id, company_id);
    if (!detail) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Сделка не найдена"));
      co_return;
    }

    Json::Value out;
    out["deal"] = dealJson(*detail);
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "getDeal db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "getDeal error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
