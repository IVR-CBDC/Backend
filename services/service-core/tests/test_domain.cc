#include <catch2/catch_test_macros.hpp>

#include <domain.h>

using namespace core_svc;

TEST_CASE("строковые значения стадий совпадают с CHECK-ограничением в БД") {
  CHECK(toString(Stage::compliance_check) == "compliance_check");
  CHECK(stageFromString("settlement") == Stage::settlement);
  CHECK_FALSE(stageFromString("nonsense").has_value());
}

TEST_CASE("display_id собирается из года и номера") {
  CHECK(displayId(2026, 143) == "DEAL-2026-0143");
  CHECK(displayId(2026, 7) == "DEAL-2026-0007");
  CHECK(displayId(2026, 12345) == "DEAL-2026-12345");
}

TEST_CASE("прогресс считается по выполненным шагам таймлайна") {
  std::vector<TimelineStep> timeline{
      {1, "Сделка создана", "Вы", StepStatus::done, ""},
      {2, "Выбор сценария расчёта", "Вы", StepStatus::done, ""},
      {3, "Документы", "Вы", StepStatus::in_progress, ""},
      {4, "Комплаенс-проверка", "Банк", StepStatus::pending, ""},
  };

  CHECK(progressPercent(timeline) == 50);
  CHECK(progressPercent({}) == 0);
}

TEST_CASE("порядок сценариев фиксирован") {
  CHECK(kScenarioOrder[0] == Scenario::cbdc);
  CHECK(kScenarioOrder[3] == Scenario::trade_finance);
}
