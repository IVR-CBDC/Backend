#include <catch2/catch_test_macros.hpp>

#include <emulator.h>

#include <cstdlib>
#include <set>
#include <string>

using namespace core_svc;

TEST_CASE("хэш стабилен и не зависит от прогона") {
  CHECK(fnv1a("deal-1") == fnv1a("deal-1"));
  CHECK(fnv1a("deal-1") != fnv1a("deal-2"));
}

TEST_CASE("исход проверки документа детерминирован по сделке и виду документа") {
  const bool first = documentApproved("11111111-1111-4111-8111-111111111111", "contract");
  for (int i = 0; i < 5; ++i)
    CHECK(documentApproved("11111111-1111-4111-8111-111111111111", "contract") == first);
}

TEST_CASE("отказы редки, но случаются") {
  int rejected = 0;
  for (int i = 0; i < 200; ++i)
    if (!documentApproved("deal-" + std::to_string(i), "contract"))
      ++rejected;

  CHECK(rejected > 0);
  CHECK(rejected < 60);  // ожидаем около 10%, ловим только грубые поломки
}

TEST_CASE("причина отказа — осмысленный текст из фиксированного набора") {
  const std::set<std::string> known{"Скан нечитаем", "Сумма не совпадает с инвойсом"};

  for (int i = 0; i < 50; ++i) {
    const auto reason = documentRejectReason("deal-" + std::to_string(i), "contract");
    CHECK(known.count(reason) == 1);
  }
}

TEST_CASE("задержки комплаенса детерминированы и нечасты") {
  const bool first = complianceDelayed("deal-7");
  CHECK(complianceDelayed("deal-7") == first);

  int delayed = 0;
  for (int i = 0; i < 200; ++i)
    if (complianceDelayed("deal-" + std::to_string(i)))
      ++delayed;

  CHECK(delayed > 0);
  CHECK(delayed < 100);
}

TEST_CASE("скорость эмулятора берётся из окружения") {
  setenv("EMULATOR_SPEED", "demo", 1);
  const auto demo = emulatorConfigFromEnv();
  CHECK(demo.doc_review_sec < 60);

  setenv("EMULATOR_SPEED", "realistic", 1);
  const auto realistic = emulatorConfigFromEnv();
  CHECK(realistic.doc_review_sec >= 60);

  unsetenv("EMULATOR_SPEED");
}
