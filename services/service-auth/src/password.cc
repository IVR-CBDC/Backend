#include "password.h"

#include <argon2.h>
#include <random>
#include <stdexcept>
#include <vector>

namespace auth_svc {

namespace {
constexpr uint32_t T_COST = 2;
constexpr uint32_t M_COST = 1 << 16;
constexpr uint32_t PARALLEL = 1;
constexpr size_t SALT_LEN = 16;
constexpr size_t HASH_LEN = 32;
} // namespace

std::string hashPassword(const std::string &plaintext) {
  std::random_device rd;
  std::vector<uint8_t> salt(SALT_LEN);
  for (auto &b : salt) {
    b = static_cast<uint8_t>(rd());
  }

  size_t enc_len = argon2_encodedlen(T_COST, M_COST, PARALLEL, SALT_LEN,
                                     HASH_LEN, Argon2_id);
  std::string encoded(enc_len, '\0');

  int rc = argon2id_hash_encoded(T_COST, M_COST, PARALLEL, plaintext.data(),
                                 plaintext.size(), salt.data(), salt.size(),
                                 HASH_LEN, encoded.data(), encoded.size());
  if (rc != ARGON2_OK) {
    throw std::runtime_error(std::string("argon2 hash failed: ") +
                             argon2_error_message(rc));
  }
  encoded.resize(std::char_traits<char>::length(encoded.c_str()));
  return encoded;
}

bool verifyPassword(const std::string &plaintext, const std::string &encoded) {
  return argon2id_verify(encoded.c_str(), plaintext.data(), plaintext.size()) ==
         ARGON2_OK;
}

} // namespace auth_svc
