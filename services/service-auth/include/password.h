#pragma once
#include <string>

namespace auth_svc {
std::string hashPassword(const std::string& plaintext);
bool verifyPassword(const std::string& plaintext, const std::string& encoded);
}
