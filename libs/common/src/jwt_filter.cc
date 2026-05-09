#include "common/jwt_filter.h"
#include "common/jwt_verifier.h"

#include <drogon/HttpResponse.h>

namespace common {

namespace {
drogon::HttpResponsePtr unauthorized(const std::string &msg) {
  Json::Value j;
  j["error"] = msg;
  auto r = drogon::HttpResponse::newHttpJsonResponse(j);
  r->setStatusCode(drogon::k401Unauthorized);
  return r;
}
}  // namespace

void JwtFilter::doFilter(const drogon::HttpRequestPtr &req,
                         drogon::FilterCallback &&fcb,
                         drogon::FilterChainCallback &&fccb) {
  auto auth = req->getHeader("Authorization");
  constexpr std::string_view prefix = "Bearer ";
  if (auth.size() < prefix.size() ||
      std::string_view(auth).substr(0, prefix.size()) != prefix) {
    fcb(unauthorized("missing bearer token"));
    return;
  }

  std::string token = auth.substr(prefix.size());
  auto claims = JwtVerifier::instance().verify(token);
  if (!claims) {
    fcb(unauthorized("invalid or expired token"));
    return;
  }

  req->attributes()->insert("user_id", claims->sub);
  fccb();
}

}  // namespace common
