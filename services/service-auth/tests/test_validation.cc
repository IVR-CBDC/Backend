#include <catch2/catch_test_macros.hpp>

#include <validation.h>

using auth_svc::RegisterInput;
using auth_svc::normalizeRegisterInput;
using auth_svc::validateRegister;

namespace {
RegisterInput valid() {
  return RegisterInput{"egor", "hunter22", "Егор", "ООО Ромашка", "7707083893"};
}
}  // namespace

TEST_CASE("корректный ввод проходит") {
  CHECK_FALSE(validateRegister(valid()).has_value());
}

TEST_CASE("логин обязателен") {
  auto input = valid();
  input.login = "   ";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->code == "VALIDATION_ERROR");
  CHECK(error->message == "Укажите логин");
}

TEST_CASE("пароль короче шести символов отклоняется") {
  auto input = valid();
  input.password = "12345";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->message == "Пароль должен быть не короче 6 символов");
}

TEST_CASE("название компании обязательно") {
  auto input = valid();
  input.company_name = "  ";
  auto error = validateRegister(input);
  REQUIRE(error.has_value());
  CHECK(error->message == "Укажите название компании");
}

TEST_CASE("normalizeRegisterInput обрезает пробелы у login, name, company_name") {
  RegisterInput input{"  egor  ", "hunter22", "  Егор  ", "  ООО Ромашка  ", "7707083893"};
  normalizeRegisterInput(input);

  CHECK(input.login == "egor");
  CHECK(input.name == "Егор");
  CHECK(input.company_name == "ООО Ромашка");
  CHECK(input.password == "hunter22");
  CHECK(input.inn == "7707083893");
}

TEST_CASE("ИНН — ровно 10 цифр") {
  for (const auto &bad : {"", "770708389", "77070838933", "77070838a3", "770 0838933"}) {
    auto input = valid();
    input.inn = bad;
    auto error = validateRegister(input);
    REQUIRE(error.has_value());
    CHECK(error->message == "ИНН юрлица состоит из 10 цифр");
  }
}
