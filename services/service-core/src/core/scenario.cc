#include "commission_client.h"
#include "core_controller.h"
#include "repository.h"

#include <algorithm>
#include <common/helpers.h>
#include <drogon/drogon.h>
#include <sstream>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

namespace {

// Body validation shared shape with documents.cc's submitDocument — kept
// duplicated (a few lines) rather than factored into a shared header, since
// the brief scopes this task to scenario.cc/documents.cc/deals.cc only.
bool parseVersion(const Json::Value &body, int &out) {
  if (!body.isMember("version") || !body["version"].isIntegral()) return false;
  int v = body["version"].asInt();
  if (v < 0) return false;
  out = v;
  return true;
}

std::string formatAmount(double amount) {
  std::ostringstream out;
  out.precision(2);
  out << std::fixed << amount;
  return out.str();
}

}  // namespace

// Подтверждение сценария расчёта. Комиссия — авторитетная, пересчитывается
// здесь через service-commission; клиентское значение (если было в форме)
// никогда не сохраняется как есть.
Task<> CoreController::chooseScenario(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> cb,
                                      std::string id) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  auto json = req->getJsonObject();
  int version = 0;
  std::optional<Scenario> scenario = json ? scenarioFromString(json->get("scenario", "").asString()) : std::nullopt;
  if (!json || !scenario || !parseVersion(*json, version)) {
    cb(jsonError(k400BadRequest, "VALIDATION_ERROR", "Укажите корректные scenario и version"));
    co_return;
  }

  try {
    auto detail = co_await DealRepository::find(id, company_id);
    if (!detail) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Сделка не найдена"));
      co_return;
    }

    // Cheap version check first: a stale retry (client still on the old
    // version) would otherwise read as an invalid transition once the deal
    // has already moved past `created`, masking the real cause. apply()
    // re-checks this under a row lock for the race window between here and
    // there — this is only the fast, unlocked rejection.
    if (version != detail->deal.version) {
      cb(jsonError(k409Conflict, "VERSION_CONFLICT", "Сделка изменилась, обновите страницу"));
      co_return;
    }

    // Check the transition before touching service-commission at all — no
    // point re-pricing a request that's going to be rejected anyway.
    auto transitionResult = onScenarioConfirmed(toDealState(*detail));
    if (std::holds_alternative<TransitionError>(transitionResult)) {
      const auto &error = std::get<TransitionError>(transitionResult);
      cb(jsonError(k409Conflict, error.code, error.message));
      co_return;
    }
    const auto &transition = std::get<Transition>(transitionResult);

    const bool isImport = detail->deal.operation_type == OperationType::import_;
    std::string from_country = isImport ? "RU" : detail->deal.counterparty_country;
    std::string to_country = isImport ? detail->deal.counterparty_country : "RU";

    auto quotesResult = co_await fetchQuotes(req->getHeader("Authorization"), from_country, to_country,
                                             detail->deal.currency, detail->deal.amount);
    if (std::holds_alternative<CommissionError>(quotesResult)) {
      const auto &error = std::get<CommissionError>(quotesResult);
      cb(jsonError(static_cast<HttpStatusCode>(error.status), error.code, error.message));
      co_return;
    }
    const auto &quotes = std::get<std::vector<Quote>>(quotesResult);

    auto it = std::find_if(quotes.begin(), quotes.end(), [&](const Quote &q) { return q.scenario == *scenario; });
    if (it == quotes.end() || !it->available) {
      std::string reason = (it != quotes.end() && !it->unavailable_reason.empty())
                                ? it->unavailable_reason
                                : "Сценарий недоступен для этого коридора";
      cb(jsonError(k409Conflict, "SCENARIO_UNAVAILABLE", reason));
      co_return;
    }

    DealMutation mutation;
    mutation.scenario = *scenario;
    mutation.commission_total = formatAmount(it->total);
    mutation.commission_breakdown = it->breakdown;

    auto result = co_await DealRepository::apply(id, company_id, version, transition, mutation);
    if (result.status == ApplyResult::Status::not_found) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Сделка не найдена"));
      co_return;
    }
    if (result.status == ApplyResult::Status::version_conflict) {
      cb(jsonError(k409Conflict, "VERSION_CONFLICT", "Сделка изменилась, обновите страницу"));
      co_return;
    }

    Json::Value out;
    out["deal"] = dealJson(*result.deal);
    cb(HttpResponse::newHttpJsonResponse(out));

  } catch (const orm::DrogonDbException &e) {
    LOG_ERROR << "chooseScenario db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "chooseScenario error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
