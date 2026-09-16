#include <catch2/catch_test_macros.hpp>

#include <state_machine.h>

using namespace core_svc;

namespace {

DealState freshDeal() {
  DealState deal;
  deal.stage = Stage::created;
  deal.timeline = initialTimeline();
  return deal;
}

DealState dealInDocuments(Scenario scenario, DocStatus status) {
  DealState deal = freshDeal();
  deal.stage = Stage::documents;
  deal.scenario = scenario;
  for (const auto &kind : requiredDocuments(scenario))
    deal.documents.push_back({kind, status});
  deal.timeline[1].status = StepStatus::done;
  deal.timeline[2].status = StepStatus::in_progress;
  return deal;
}

const Transition &transition(const TransitionResult &result) {
  REQUIRE(std::holds_alternative<Transition>(result));
  return std::get<Transition>(result);
}

const TransitionError &error(const TransitionResult &result) {
  REQUIRE(std::holds_alternative<TransitionError>(result));
  return std::get<TransitionError>(result);
}

}  // namespace

TEST_CASE("изначальный таймлайн — семь шагов, первый выполнен") {
  const auto timeline = initialTimeline();
  REQUIRE(timeline.size() == 7);
  CHECK(timeline[0].status == StepStatus::done);
  CHECK(timeline[0].actor == "Вы");
  CHECK(timeline[6].step == "Завершение сделки");
  for (size_t i = 1; i < timeline.size(); ++i)
    CHECK(timeline[i].status == StepStatus::pending);
}

TEST_CASE("набор обязательных документов зависит от сценария") {
  CHECK(requiredDocuments(Scenario::cbdc) == std::vector<std::string>{"contract", "invoice"});
  CHECK(requiredDocuments(Scenario::bank_transfer) ==
        std::vector<std::string>{"contract", "invoice", "deal_passport"});
  CHECK(requiredDocuments(Scenario::trade_finance) ==
        std::vector<std::string>{"contract", "invoice", "deal_passport", "letter_of_credit"});
  CHECK(requiredDocuments(Scenario::smart_contract) ==
        std::vector<std::string>{"contract", "invoice", "digital_signature"});
}

TEST_CASE("подтверждение сценария переводит сделку к документам") {
  const auto result = transition(onScenarioConfirmed(freshDeal()));

  CHECK(result.stage == Stage::documents);
  CHECK(result.timeline.size() == 2);
  CHECK(result.timeline[0].seq == 2);
  CHECK(result.timeline[0].status == StepStatus::done);
  CHECK(result.timeline[1].seq == 3);
  CHECK(result.timeline[1].status == StepStatus::in_progress);
  CHECK(result.schedule_next == false);  // ждём документы от пользователя
}

TEST_CASE("сценарий нельзя подтвердить дважды после подачи документов") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::uploaded);

  CHECK(error(onScenarioConfirmed(deal)).code == "INVALID_TRANSITION");
}

TEST_CASE("подача документа ставит его в очередь на проверку") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::missing);

  const auto result = transition(onDocumentSubmitted(deal, "contract"));

  CHECK(result.stage == Stage::documents);
  CHECK(result.schedule_next == true);  // дальше работает эмулятор
}

TEST_CASE("нельзя подать документ, которого нет у сделки") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::missing);

  CHECK(error(onDocumentSubmitted(deal, "letter_of_credit")).code == "INVALID_TRANSITION");
}

TEST_CASE("нельзя переподать уже одобренный документ") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);

  CHECK(error(onDocumentSubmitted(deal, "contract")).code == "INVALID_TRANSITION");
}

TEST_CASE("одобрение последнего документа двигает сделку в комплаенс") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);
  deal.documents[1].status = DocStatus::under_review;

  const auto result = transition(onDocumentReviewed(deal, "invoice", true, ""));

  CHECK(result.stage == Stage::compliance_check);
  CHECK(result.schedule_next == true);
  REQUIRE(result.notifications.size() == 1);
  CHECK(result.notifications[0].severity == "info");
}

TEST_CASE("одобрение не последнего документа оставляет сделку на документах") {
  auto deal = dealInDocuments(Scenario::bank_transfer, DocStatus::missing);
  deal.documents[0].status = DocStatus::under_review;

  const auto result = transition(onDocumentReviewed(deal, "contract", true, ""));

  CHECK(result.stage == Stage::documents);
}

TEST_CASE("отклонение документа блокирует сделку с причиной") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::under_review);

  const auto result = transition(onDocumentReviewed(deal, "invoice", false, "Скан нечитаем"));

  CHECK(result.stage == Stage::blocked);
  CHECK(result.blocked_from == Stage::documents);
  CHECK(result.blocker_reason == "Скан нечитаем");
  REQUIRE(result.notifications.size() == 1);
  CHECK(result.notifications[0].severity == "critical");
}

TEST_CASE("переподача отклонённого документа возвращает сделку к документам") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);
  deal.stage = Stage::blocked;
  deal.blocked_from = Stage::documents;
  deal.documents[1].status = DocStatus::rejected;

  const auto result = transition(onDocumentSubmitted(deal, "invoice"));

  CHECK(result.stage == Stage::documents);
  CHECK_FALSE(result.blocked_from.has_value());
  CHECK(result.schedule_next == true);
}

TEST_CASE("успешный комплаенс переводит к расчёту") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);
  deal.stage = Stage::compliance_check;
  deal.timeline[2].status = StepStatus::done;
  deal.timeline[3].status = StepStatus::in_progress;

  const auto result = transition(onComplianceResult(deal, true, ""));

  CHECK(result.stage == Stage::settlement);
  CHECK(result.schedule_next == true);
}

TEST_CASE("отказ комплаенса блокирует сделку") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);
  deal.stage = Stage::compliance_check;

  const auto result = transition(onComplianceResult(deal, false, "ФНС не подтвердила контракт"));

  CHECK(result.stage == Stage::blocked);
  CHECK(result.blocked_from == Stage::compliance_check);
  CHECK(result.blocker_reason == "ФНС не подтвердила контракт");
}

TEST_CASE("завершение расчёта закрывает сделку") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);
  deal.stage = Stage::settlement;

  const auto result = transition(onSettlementDone(deal));

  CHECK(result.stage == Stage::completed);
  CHECK(result.schedule_next == false);
  REQUIRE(result.notifications.size() == 1);
}

TEST_CASE("нельзя завершить расчёт из стадии документов") {
  auto deal = dealInDocuments(Scenario::cbdc, DocStatus::approved);

  CHECK(error(onSettlementDone(deal)).code == "INVALID_TRANSITION");
}
