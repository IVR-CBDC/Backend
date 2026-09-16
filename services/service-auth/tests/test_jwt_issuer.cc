#include <catch2/catch_test_macros.hpp>

#include <common/jwt_verifier.h>
#include <jwt_issuer.h>

// Выпуск и проверка используют разные API libjwt (builder против checker),
// поэтому round-trip гоняем на настоящей паре ключей.
TEST_CASE("выпущенный токен проходит верификацию и несёт company_id") {
  const auto token = auth_svc::JwtIssuer::instance().issue("user-42", "company-7");

  auto claims = common::JwtVerifier::instance().verify(token);

  REQUIRE(claims.has_value());
  CHECK(claims->sub == "user-42");
  CHECK(claims->company_id == "company-7");
}

TEST_CASE("истёкший токен не проходит верификацию") {
  const auto token = auth_svc::JwtIssuer::instance().issue("user-42", "company-7", -60);

  CHECK_FALSE(common::JwtVerifier::instance().verify(token).has_value());
}

TEST_CASE("issue с ttl_seconds = 0 бросает исключение") {
  REQUIRE_THROWS_AS(
      auth_svc::JwtIssuer::instance().issue("user-42", "company-7", 0),
      std::invalid_argument);
}
