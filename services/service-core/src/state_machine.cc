#include "state_machine.h"

#include <algorithm>

namespace core_svc {

namespace {

TransitionError invalidTransition(std::string message) {
  return TransitionError{"INVALID_TRANSITION", std::move(message)};
}

// Actor for the scenario-specific 6th timeline step. Not part of the public
// interface (only the step label is, via scenarioStepLabel); the repository
// pairs the two when it rewrites step 6 after a scenario is chosen.
std::string scenarioStepActor(Scenario scenario) {
  switch (scenario) {
    case Scenario::cbdc: return "Платформа ЦВЦБ";
    case Scenario::bank_transfer: return "Банк";
    case Scenario::smart_contract: return "Смарт-контракт";
    case Scenario::trade_finance: return "Банк-гарант";
  }
  return "";
}

const DocumentState *findDocument(const DealState &deal, const std::string &kind) {
  auto it = std::find_if(deal.documents.begin(), deal.documents.end(),
                          [&](const DocumentState &doc) { return doc.kind == kind; });
  return it == deal.documents.end() ? nullptr : &*it;
}

// True when every required document is approved, treating `kind` as already
// approved even though its stored status in `deal` (still under_review) has
// not been persisted yet.
bool allRequiredApprovedIfReviewed(const DealState &deal, const std::string &kind) {
  if (!deal.scenario) return false;
  for (const auto &required : requiredDocuments(*deal.scenario)) {
    const DocumentState *doc = findDocument(deal, required);
    if (!doc) return false;
    const DocStatus effective = doc->kind == kind ? DocStatus::approved : doc->status;
    if (effective != DocStatus::approved) return false;
  }
  return true;
}

}  // namespace

std::vector<TimelineStep> initialTimeline() {
  return {
      {1, "Сделка создана", "Вы", StepStatus::done, "", "", ""},
      {2, "Выбор сценария расчёта", "Вы", StepStatus::pending, "", "", ""},
      {3, "Документы", "Вы", StepStatus::pending, "", "", ""},
      {4, "Комплаенс-проверка", "Банк", StepStatus::pending, "", "", ""},
      {5, "Проверка ФНС", "ФНС", StepStatus::pending, "", "", ""},
      {6, "Расчёт", "Банк", StepStatus::pending, "", "", ""},
      {7, "Завершение сделки", "Система", StepStatus::pending, "", "", ""},
  };
}

std::vector<std::string> requiredDocuments(Scenario scenario) {
  switch (scenario) {
    case Scenario::cbdc:
      return {"contract", "invoice"};
    case Scenario::bank_transfer:
      return {"contract", "invoice", "deal_passport"};
    case Scenario::smart_contract:
      return {"contract", "invoice", "digital_signature"};
    case Scenario::trade_finance:
      return {"contract", "invoice", "deal_passport", "letter_of_credit"};
  }
  return {};
}

std::string scenarioStepLabel(Scenario scenario) {
  switch (scenario) {
    case Scenario::cbdc: return "Расчёт через ЦВЦБ";
    case Scenario::bank_transfer: return "Банковский перевод";
    case Scenario::smart_contract: return "Исполнение смарт-контракта";
    case Scenario::trade_finance: return "Раскрытие аккредитива";
  }
  return "";
}

TransitionResult onScenarioConfirmed(const DealState &deal) {
  if (deal.stage != Stage::created)
    return invalidTransition("Сценарий можно выбрать только для новой сделки");

  Transition t;
  t.stage = Stage::documents;
  t.timeline = {{2, StepStatus::done, ""}, {3, StepStatus::in_progress, ""}};
  t.schedule_next = false;  // ждём документы от пользователя
  return t;
}

TransitionResult onDocumentSubmitted(const DealState &deal, const std::string &kind) {
  const bool wasBlocked = deal.stage == Stage::blocked && deal.blocked_from == Stage::documents;
  const bool inDocumentsFlow = deal.stage == Stage::documents || wasBlocked;
  if (!inDocumentsFlow) return invalidTransition("Сделка не принимает документы на этой стадии");

  const DocumentState *doc = findDocument(deal, kind);
  if (!doc) return invalidTransition("Документ не входит в список требуемых для этой сделки");
  if (doc->status != DocStatus::missing && doc->status != DocStatus::rejected)
    return invalidTransition("Документ уже подан и ожидает или прошёл проверку");

  Transition t;
  t.stage = Stage::documents;
  t.schedule_next = true;  // дальше документ проверяет эмулятор
  // Unblocking resubmission: a sibling document may have been left stranded
  // in uploaded/under_review with next_action_at cleared when the deal first
  // blocked (see reviewDocument's clearDocumentAction in emulator.cc) — tell
  // apply() to re-arm it now that the deal is moving again.
  t.rearm_pending_documents = wasBlocked;
  return t;
}

TransitionResult onDocumentReviewed(const DealState &deal, const std::string &kind, bool approved,
                                     const std::string &reject_reason) {
  if (deal.stage != Stage::documents) return invalidTransition("Сделка не находится на проверке документов");

  const DocumentState *doc = findDocument(deal, kind);
  if (!doc) return invalidTransition("Документ не входит в список требуемых для этой сделки");
  if (doc->status != DocStatus::under_review) return invalidTransition("Документ не был подан на проверку");

  if (!approved) {
    Transition t;
    t.stage = Stage::blocked;
    t.blocked_from = Stage::documents;
    t.blocker_reason = reject_reason;
    t.notifications = {{"critical", reject_reason}};
    t.schedule_next = false;  // ждём, пока пользователь переподаст документ
    return t;
  }

  if (!allRequiredApprovedIfReviewed(deal, kind)) {
    Transition t;
    t.stage = Stage::documents;
    t.schedule_next = false;  // ждём остальные документы
    return t;
  }

  Transition t;
  t.stage = Stage::compliance_check;
  t.timeline = {{3, StepStatus::done, ""}, {4, StepStatus::in_progress, ""}};
  t.notifications = {{"info", "Документы приняты, идёт комплаенс-проверка"}};
  t.schedule_next = true;  // дальше комплаенс проверяет эмулятор
  return t;
}

TransitionResult onComplianceResult(const DealState &deal, bool approved, const std::string &reason) {
  if (deal.stage != Stage::compliance_check) return invalidTransition("Сделка не находится на комплаенс-проверке");

  if (!approved) {
    Transition t;
    t.stage = Stage::blocked;
    t.blocked_from = Stage::compliance_check;
    t.blocker_reason = reason;
    t.notifications = {{"critical", reason}};
    t.schedule_next = false;
    return t;
  }

  Transition t;
  t.stage = Stage::settlement;
  t.timeline = {{4, StepStatus::done, ""}, {5, StepStatus::done, ""}, {6, StepStatus::in_progress, ""}};
  t.schedule_next = true;  // дальше расчёт ведёт эмулятор
  return t;
}

TransitionResult onComplianceDelayed(const DealState &deal) {
  if (deal.stage != Stage::compliance_check)
    return invalidTransition("Сделку можно поставить на паузу ФНС только во время комплаенс-проверки");

  Transition t;
  t.stage = Stage::compliance_check;  // стадия не меняется, это пауза внутри неё
  // Step 5 ("Проверка ФНС") — see initialTimeline(). No notification: the
  // pause is visible in the timeline (status delayed + delay_reason) and now
  // fires a deal.updated event on its own (that's the whole point of this
  // transition), so a separate notification would just be noise for
  // something that resolves itself within compliance_sec.
  t.timeline = {{5, StepStatus::delayed, "Ожидаем ответ от ФНС"}};
  t.schedule_next = true;  // дальше комплаенс проверяет эмулятор ещё раз
  return t;
}

TransitionResult onSettlementDone(const DealState &deal) {
  if (deal.stage != Stage::settlement) return invalidTransition("Сделка не находится на стадии расчёта");

  Transition t;
  t.stage = Stage::completed;
  t.timeline = {{6, StepStatus::done, ""}, {7, StepStatus::done, ""}};
  t.notifications = {{"info", "Сделка завершена"}};
  t.schedule_next = false;
  return t;
}

}  // namespace core_svc
