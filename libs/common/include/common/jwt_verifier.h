#pragma once

#include <string>
#include <optional>
#include <jwt.h>

namespace common {

struct Claims {
    std::string sub;         // user_id
    std::string company_id;  // юрлицо, от имени которого работает пользователь
    std::string iss;
    std::string aud;
    long long exp = 0;       // unix time истечения; 0 значит claim отсутствовал в токене
};

class JwtVerifier {
public:
    static JwtVerifier& instance();
    ~JwtVerifier();

    std::optional<Claims> verify(const std::string& token);

private:
    JwtVerifier();
    jwk_set_t* jwk_set_ = nullptr;
    const jwk_item_t* key_ = nullptr;
};

}  // namespace common
