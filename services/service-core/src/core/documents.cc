#include "core_controller.h"
#include "repository.h"

#include <algorithm>
#include <common/helpers.h>
#include <cstdlib>
#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

namespace {

bool parseVersion(const Json::Value &body, int &out) {
  if (!body.isMember("version") || !body["version"].isIntegral()) return false;
  int v = body["version"].asInt();
  if (v < 0) return false;
  out = v;
  return true;
}

// Delay before the emulator reviews a submitted document. Task 5 moves this
// to EmulatorConfig::doc_review_sec; reading the env var here keeps that a
// one-line change later instead of threading a config object through today.
long long docReviewDelaySec() {
  const char *env = std::getenv("EMULATOR_DOC_DELAY_SEC");
  if (!env || !*env) return 5;
  try {
    return std::stoll(env);
  } catch (...) {
    return 5;
  }
}

}  // namespace

// Подача документа на проверку. `docId` в пути — это id строки
// deal_documents, а не kind: ищем документ сделки по id, затем работаем с
// его kind дальше (состояние, мутация).
Task<> CoreController::submitDocument(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> cb,
                                      std::string dealId, std::string docId) {
  auto company_id = req->attributes()->get<std::string>("company_id");

  auto json = req->getJsonObject();
  int version = 0;
  if (!json || !parseVersion(*json, version)) {
    cb(jsonError(k400BadRequest, "VALIDATION_ERROR", "Укажите корректный version"));
    co_return;
  }

  try {
    auto detail = co_await DealRepository::find(dealId, company_id);
    if (!detail) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Сделка не найдена"));
      co_return;
    }

    auto docIt = std::find_if(detail->documents.begin(), detail->documents.end(),
                              [&](const DocumentRow &d) { return d.id == docId; });
    if (docIt == detail->documents.end()) {
      cb(jsonError(k404NotFound, "NOT_FOUND", "Документ не найден"));
      co_return;
    }

    // See scenario.cc's chooseScenario for why this runs before the
    // transition check: a stale version reads as an invalid transition once
    // the deal has moved on, masking the real cause.
    if (version != detail->deal.version) {
      cb(jsonError(k409Conflict, "VERSION_CONFLICT", "Сделка изменилась, обновите страницу"));
      co_return;
    }

    auto transitionResult = onDocumentSubmitted(toDealState(*detail), docIt->kind);
    if (std::holds_alternative<TransitionError>(transitionResult)) {
      const auto &error = std::get<TransitionError>(transitionResult);
      cb(jsonError(k409Conflict, error.code, error.message));
      co_return;
    }
    const auto &transition = std::get<Transition>(transitionResult);

    DealMutation mutation;
    mutation.document_kind = docIt->kind;
    mutation.document_status = DocStatus::uploaded;
    mutation.next_action_in_sec = docReviewDelaySec();

    auto result = co_await DealRepository::apply(dealId, company_id, version, transition, mutation);
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
    LOG_ERROR << "submitDocument db error: " << e.base().what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (const std::exception &e) {
    LOG_ERROR << "submitDocument error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
