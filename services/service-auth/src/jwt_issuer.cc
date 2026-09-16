#include "jwt_issuer.h"
#include <cstdlib>
#include <ctime>
#include <jwt.h>
#include <memory>
#include <stdexcept>

namespace auth_svc {

JwtIssuer &JwtIssuer::instance() {
  static JwtIssuer inst;
  return inst;
}

JwtIssuer::JwtIssuer() {
  const char *path = std::getenv("JWT_PRIVATE_KEY_PATH");
  if (!path)
    throw std::runtime_error("JWT_PRIVATE_KEY_PATH not set");

  jwk_set_ = jwks_create_fromfile(path);
  if (!jwk_set_ || jwks_error(jwk_set_))
    throw std::runtime_error("failed to load JWK from " + std::string(path));

  key_ = jwks_item_get(jwk_set_, 0);
  if (!key_ || jwks_item_error(key_))
    throw std::runtime_error("invalid JWK key in " + std::string(path));
}

JwtIssuer::~JwtIssuer() {
  if (jwk_set_)
    jwks_free(jwk_set_);
}

std::string JwtIssuer::issue(const std::string &user_id,
                             const std::string &company_id, int ttl_seconds) {
  jwt_builder_t *builder = jwt_builder_new();
  if (!builder)
    throw std::runtime_error("jwt_builder_new failed");

  auto cleanup = std::unique_ptr<jwt_builder_t, decltype(&jwt_builder_free)>(
      builder, jwt_builder_free);

  if (jwt_builder_setkey(builder, JWT_ALG_RS256, key_) != 0) {
    throw std::runtime_error(
        std::string("jwt_builder_setkey failed: ") +
        jwt_builder_error_msg(builder));
  }

  jwt_builder_enable_iat(builder, 1);
  // jwt_builder_time_offset трактует secs <= 0 как «не добавлять claim вовсе»
  // (см. testing_support::makeToken), поэтому уже истёкший токен (ttl < 0)
  // выставляем как exp в прошлом напрямую через claim_set.
  if (ttl_seconds > 0) {
    jwt_builder_time_offset(builder, JWT_CLAIM_EXP,
                            static_cast<time_t>(ttl_seconds));
  } else if (ttl_seconds < 0) {
    jwt_value_t exp_val{};
    exp_val.type = JWT_VALUE_INT;
    exp_val.name = "exp";
    exp_val.int_val = static_cast<jwt_long_t>(std::time(nullptr) + ttl_seconds);
    jwt_builder_claim_set(builder, &exp_val);
  }

  jwt_value_t jval{};

  jval.type = JWT_VALUE_STR;
  jval.name = "sub";
  jval.str_val = user_id.c_str();
  jwt_builder_claim_set(builder, &jval);

  jval.type = JWT_VALUE_STR;
  jval.name = "company_id";
  jval.str_val = company_id.c_str();
  jwt_builder_claim_set(builder, &jval);

  jval.type = JWT_VALUE_STR;
  jval.name = "iss";
  jval.str_val = "service-auth";
  jwt_builder_claim_set(builder, &jval);

  jval.type = JWT_VALUE_STR;
  jval.name = "aud";
  jval.str_val = "internal";
  jwt_builder_claim_set(builder, &jval);

  char *encoded = jwt_builder_generate(builder);
  if (!encoded)
    throw std::runtime_error(
        std::string("jwt_builder_generate failed: ") +
        jwt_builder_error_msg(builder));

  std::string result(encoded);
  std::free(encoded);
  return result;
}

} // namespace auth_svc
