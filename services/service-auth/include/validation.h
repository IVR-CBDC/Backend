#pragma once

#include <optional>
#include <string>

namespace auth_svc {

struct RegisterInput {
  std::string login;
  std::string password;
  std::string name;
  std::string company_name;
  std::string inn;
};

struct ValidationError {
  std::string code;
  std::string message;
};

// Чистая проверка полей: без БД и HTTP, поэтому покрыта юнит-тестами.
std::optional<ValidationError> validateRegister(const RegisterInput &input);

// Приводит поля к каноничному виду (trim) — вызывать перед записью в БД.
void normalizeRegisterInput(RegisterInput &input);

}  // namespace auth_svc
