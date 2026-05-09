#include "jwt_issuer.h"
#include <cstdlib>
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

std::string JwtIssuer::issue(const std::string &user_id, int ttl_seconds) {
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
  jwt_builder_time_offset(builder, JWT_CLAIM_EXP,
                          static_cast<time_t>(ttl_seconds));

  jwt_value_t jval{};

  jval.type = JWT_VALUE_STR;
  jval.name = "sub";
  jval.str_val = user_id.c_str();
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
