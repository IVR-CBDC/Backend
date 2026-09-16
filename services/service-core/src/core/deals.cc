#include "core_controller.h"
#include "repository.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <common/helpers.h>
#include <drogon/drogon.h>
#include <sstream>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

namespace {

// true when `s` is exactly `len` ASCII letters; also uppercases it in place,
// matching the brief's "приводится к верхнему регистру" for country/currency
// codes.
bool upperAlpha(std::string &s, size_t len) {
  if (s.size() != len) return false;
  for (char &c : s) {
    if (!std::isalpha(static_cast<unsigned char>(c))) return false;
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return true;
}

std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

// Formats a validated amount as the canonical 2-decimal NUMERIC text that
// DealRow::amount expects (see repository.h) — never stored as a double.
std::string formatAmount(double amount) {
  std::ostringstream out;
  out.precision(2);
  out << std::fixed << amount;
  return out.str();
}

struct CreateDealInput {
  std::string counterparty_country;
  std::string counterparty_name;
  OperationType operation_type;
  std::string amount;  // canonical NUMERIC text
  std::string currency;
};

// Returns the validation error message, or empty on success (out is filled
// only on success). One field at a time so the user gets an actionable
// message instead of a generic "bad request".
std::string validateCreateDeal(const Json::Value &body, CreateDealInput &out) {
  std::string country = body.get("counterparty_country", "").asString();
  if (!upperAlpha(country, 2)) return "Код страны должен состоять из двух букв";

  std::string name = trim(body.get("counterparty_name", "").asString());
  if (name.empty()) return "Укажите название контрагента";

  auto operation_type = operationTypeFromString(body.get("operation_type", "").asString());
  if (!operation_type) return "operation_type должен быть import или export";

  const Json::Value &amountJson = body["amount"];
  // jsoncpp's isNumeric() is true for booleanValue too (a bool converts
  // trivially to 0/1) — isBool() must be checked explicitly first, or
  // {"amount": true} would silently validate as 1.00 (F7).
  if (amountJson.isBool() || !amountJson.isNumeric()) return "Сумма должна быть числом больше нуля";
  double amount = amountJson.asDouble();
  double scaled = amount * 100.0;
  if (amount <= 0 || std::abs(scaled - std::round(scaled)) > 1e-6)
    return "Сумма должна быть больше нуля и содержать не более двух знаков после запятой";
  // NUMERIC(18,2)'s magnitude limit: 18 total digits, 2 after the decimal,
  // so the integer part tops out at 16 digits — an amount at or above 10^16
  // would blow up inside create()'s INSERT with a Postgres numeric overflow
  // (500) instead of this 400 (F6).
  if (amount >= 1e16) return "Сумма превышает максимально допустимое значение";

  std::string currency = body.get("currency", "").asString();
  if (!upperAlpha(currency, 3)) return "Код валюты должен состоять из трёх букв";

  out = CreateDealInput{country, name, *operation_type, formatAmount(amount), currency};
  return "";
}

}  // namespace

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

// Создание сделки: только персистентность, никакого сценария и никакой
// комиссии ещё — оба появляются при chooseScenario (репрайсится на сервере
// там, не здесь).
Task<> CoreController::createDeal(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> cb) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  auto json = req->getJsonObject();
  if (!json) {
    cb(jsonError(k400BadRequest, "VALIDATION_ERROR", "Некорректное тело запроса"));
    co_return;
  }

  CreateDealInput input;
  if (auto error = validateCreateDeal(*json, input); !error.empty()) {
    cb(jsonError(k400BadRequest, "VALIDATION_ERROR", error));
    co_return;
  }

  try {
    auto detail = co_await DealRepository::create(company_id, input.counterparty_country, input.counterparty_name,
                                                   input.operation_type, input.amount, input.currency);

    Json::Value out;
    out["deal"] = dealJson(detail);
    auto resp = HttpResponse::newHttpJsonResponse(out);
    resp->setStatusCode(k201Created);
    cb(resp);

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "createDeal db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "createDeal error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
