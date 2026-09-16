#pragma once

// Выпуск произвольных JWT в тестах: нужен, чтобы проверять реакцию верификатора
// на чужую подпись, истёкший срок и отсутствующие claims.

#include <jwt.h>

#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace testing_support {

struct TokenSpec {
  std::string sub = "11111111-1111-4111-8111-111111111111";
  std::optional<std::string> company_id = "22222222-2222-4222-8222-222222222222";
  std::string iss = "service-auth";
  std::string aud = "internal";
  // Положительное => TTL в секундах от текущего момента.
  // Отрицательное => токен уже истёк (exp в прошлом).
  // 0 => claim exp вовсе не добавляется в токен (для теста «токен без exp
  // отклоняется»; в проде JwtIssuer::issue бросает исключение на ttl == 0).
  int ttl_seconds = 300;
};

inline std::string makeToken(const std::string &private_jwk_path, const TokenSpec &spec = {}) {
  jwk_set_t *set = jwks_create_fromfile(private_jwk_path.c_str());
  if (!set || jwks_error(set))
    throw std::runtime_error("не удалось прочитать JWK: " + private_jwk_path);
  auto set_guard = std::unique_ptr<jwk_set_t, decltype(&jwks_free)>(set, jwks_free);

  const jwk_item_t *key = jwks_item_get(set, 0);
  if (!key || jwks_item_error(key))
    throw std::runtime_error("некорректный JWK: " + private_jwk_path);

  jwt_builder_t *builder = jwt_builder_new();
  if (!builder)
    throw std::runtime_error("jwt_builder_new failed");
  auto guard = std::unique_ptr<jwt_builder_t, decltype(&jwt_builder_free)>(builder, jwt_builder_free);

  if (jwt_builder_setkey(builder, JWT_ALG_RS256, key) != 0)
    throw std::runtime_error("jwt_builder_setkey failed");

  jwt_builder_enable_iat(builder, 1);
  // jwt_builder_time_offset трактует secs <= 0 как «не добавлять claim вовсе»,
  // поэтому отрицательный ttl (уже истёкший токен) выставляем как exp в
  // прошлом напрямую через claim_set, а не через time_offset.
  if (spec.ttl_seconds >= 0) {
    jwt_builder_time_offset(builder, JWT_CLAIM_EXP, static_cast<time_t>(spec.ttl_seconds));
  } else {
    jwt_value_t exp_val{};
    exp_val.type = JWT_VALUE_INT;
    exp_val.name = "exp";
    exp_val.int_val = static_cast<jwt_long_t>(std::time(nullptr) + spec.ttl_seconds);
    jwt_builder_claim_set(builder, &exp_val);
  }

  const auto set_str = [&](const char *name, const std::string &value) {
    jwt_value_t jval{};
    jval.type = JWT_VALUE_STR;
    jval.name = name;
    jval.str_val = value.c_str();
    jwt_builder_claim_set(builder, &jval);
  };

  set_str("sub", spec.sub);
  set_str("iss", spec.iss);
  set_str("aud", spec.aud);
  if (spec.company_id)
    set_str("company_id", *spec.company_id);

  char *encoded = jwt_builder_generate(builder);
  if (!encoded)
    throw std::runtime_error("jwt_builder_generate failed");
  std::string result(encoded);
  std::free(encoded);
  return result;
}

}  // namespace testing_support
