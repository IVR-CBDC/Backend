#include "common/jwt_verifier.h"

#include <jwt.h>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace common {

namespace {

// Callback вызывается checker-ом после парсинга токена, но до проверки подписи.
// Извлекаем claims из jwt_t и сохраняем в контекст.
int extract_claims(jwt_t *jwt, jwt_config_t *config) {
    auto *claims = static_cast<Claims *>(config->ctx);

    jwt_value_t jval{};

    jval.type = JWT_VALUE_STR;
    jval.name = "sub";
    jval.str_val = nullptr;
    jval.error = JWT_VALUE_ERR_NONE;
    if (jwt_claim_get(jwt, &jval) == JWT_VALUE_ERR_NONE && jval.str_val)
        claims->sub = jval.str_val;

    jval.type = JWT_VALUE_STR;
    jval.name = "iss";
    jval.str_val = nullptr;
    jval.error = JWT_VALUE_ERR_NONE;
    if (jwt_claim_get(jwt, &jval) == JWT_VALUE_ERR_NONE && jval.str_val)
        claims->iss = jval.str_val;

    jval.type = JWT_VALUE_STR;
    jval.name = "aud";
    jval.str_val = nullptr;
    jval.error = JWT_VALUE_ERR_NONE;
    if (jwt_claim_get(jwt, &jval) == JWT_VALUE_ERR_NONE && jval.str_val)
        claims->aud = jval.str_val;

    return 0;
}

}  // namespace

JwtVerifier& JwtVerifier::instance() {
    static JwtVerifier inst;
    return inst;
}

JwtVerifier::JwtVerifier() {
    const char* path = std::getenv("JWT_PUBLIC_KEY_PATH");
    if (!path) throw std::runtime_error("JWT_PUBLIC_KEY_PATH not set");

    jwk_set_ = jwks_create_fromfile(path);
    if (!jwk_set_ || jwks_error(jwk_set_))
        throw std::runtime_error("failed to load JWK from " + std::string(path));

    key_ = jwks_item_get(jwk_set_, 0);
    if (!key_ || jwks_item_error(key_))
        throw std::runtime_error("invalid JWK key in " + std::string(path));
}

JwtVerifier::~JwtVerifier() {
    if (jwk_set_)
        jwks_free(jwk_set_);
}

std::optional<Claims> JwtVerifier::verify(const std::string& token) {
    jwt_checker_t* checker = jwt_checker_new();
    if (!checker) return std::nullopt;

    auto guard = std::unique_ptr<jwt_checker_t, decltype(&jwt_checker_free)>(
        checker, jwt_checker_free);

    if (jwt_checker_setkey(checker, JWT_ALG_RS256, key_) != 0)
        return std::nullopt;

    jwt_checker_claim_set(checker, JWT_CLAIM_ISS, "service-auth");
    jwt_checker_claim_set(checker, JWT_CLAIM_AUD, "internal");

    Claims c;
    jwt_checker_setcb(checker, extract_claims, &c);

    if (jwt_checker_verify(checker, token.c_str()) != 0)
        return std::nullopt;

    if (c.sub.empty()) return std::nullopt;

    return c;
}

}  // namespace common
