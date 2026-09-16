#include "common/jwt_filter.h"
#include "common/jwt_verifier.h"
#include "common/helpers.h"

#include <drogon/HttpResponse.h>

namespace common {

void JwtFilter::doFilter(const drogon::HttpRequestPtr &req,
                         drogon::FilterCallback &&fcb,
                         drogon::FilterChainCallback &&fccb) {
  auto auth = req->getHeader("Authorization");
  constexpr std::string_view prefix = "Bearer ";
  if (auth.size() < prefix.size() ||
      std::string_view(auth).substr(0, prefix.size()) != prefix) {
    fcb(jsonError(drogon::k401Unauthorized, "UNAUTHORIZED",
                  "Отсутствует токен авторизации"));
    return;
  }

  std::string token = auth.substr(prefix.size());
  auto claims = JwtVerifier::instance().verify(token);
  if (!claims) {
    fcb(jsonError(drogon::k401Unauthorized, "INVALID_TOKEN",
                  "Недействительный или истёкший токен"));
    return;
  }

  req->attributes()->insert("user_id", claims->sub);
  req->attributes()->insert("company_id", claims->company_id);
  fccb();
}

}  // namespace common
