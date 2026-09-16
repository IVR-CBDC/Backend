#include <catch2/catch_test_macros.hpp>

#include <common/jwt_verifier.h>
#include <testing/token_factory.h>

using testing_support::TokenSpec;
using testing_support::makeToken;

namespace {
const std::string kOwnKey = std::string(TEST_KEYS_A) + "/jwt_private.jwk";
const std::string kForeignKey = std::string(TEST_KEYS_B) + "/jwt_private.jwk";
}  // namespace

TEST_CASE("валидный токен разбирается и отдаёт sub") {
  TokenSpec spec;
  spec.sub = "user-1";

  auto claims = common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec));

  REQUIRE(claims.has_value());
  CHECK(claims->sub == "user-1");
  CHECK(claims->iss == "service-auth");
  CHECK(claims->aud == "internal");
}

TEST_CASE("чужая подпись отклоняется") {
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kForeignKey)).has_value());
}

TEST_CASE("истёкший токен отклоняется") {
  TokenSpec spec;
  spec.ttl_seconds = -60;

  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, spec)).has_value());
}

TEST_CASE("чужой issuer или audience отклоняются") {
  TokenSpec foreign_iss;
  foreign_iss.iss = "someone-else";
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, foreign_iss)).has_value());

  TokenSpec foreign_aud;
  foreign_aud.aud = "public";
  CHECK_FALSE(common::JwtVerifier::instance().verify(makeToken(kOwnKey, foreign_aud)).has_value());
}

TEST_CASE("мусор вместо токена отклоняется") {
  CHECK_FALSE(common::JwtVerifier::instance().verify("не.токен.вовсе").has_value());
}
