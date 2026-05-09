#pragma once
#include <drogon/HttpFilter.h>

namespace common {

class JwtFilter : public drogon::HttpFilter<JwtFilter> {
public:
  void doFilter(const drogon::HttpRequestPtr &req, drogon::FilterCallback &&fcb,
                drogon::FilterChainCallback &&fccb) override;
};

}  // namespace common
