#pragma once

#include <string>
#include <jwt.h>

namespace auth_svc {

class JwtIssuer {
public:
  static JwtIssuer &instance();
  ~JwtIssuer();

  std::string issue(const std::string &user_id,
                    int ttl_seconds = 7 * 24 * 3600);

private:
  JwtIssuer();
  jwk_set_t *jwk_set_ = nullptr;
  const jwk_item_t *key_ = nullptr;
};

} // namespace auth_svc
