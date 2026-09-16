#include "validation.h"

#include <algorithm>
#include <cctype>

namespace auth_svc {

namespace {

std::string trim(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\n\r");
  if (begin == std::string::npos)
    return {};
  const auto end = value.find_last_not_of(" \t\n\r");
  return value.substr(begin, end - begin + 1);
}

bool isInn(const std::string &value) {
  return value.size() == 10 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

ValidationError error(std::string message) {
  return ValidationError{"VALIDATION_ERROR", std::move(message)};
}

}  // namespace

void normalizeRegisterInput(RegisterInput &input) {
  input.login = trim(input.login);
  input.name = trim(input.name);
  input.company_name = trim(input.company_name);
  // password и inn не трогаем: inn уже проверяется как «только цифры»,
  // а пароль — сырые байты пользователя.
}

std::optional<ValidationError> validateRegister(const RegisterInput &input) {
  if (trim(input.login).empty())
    return error("Укажите логин");
  if (input.password.size() < 6)
    return error("Пароль должен быть не короче 6 символов");
  if (trim(input.company_name).empty())
    return error("Укажите название компании");
  if (!isInn(input.inn))
    return error("ИНН юрлица состоит из 10 цифр");
  return std::nullopt;
}

}  // namespace auth_svc
