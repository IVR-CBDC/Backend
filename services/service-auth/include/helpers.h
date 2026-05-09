#pragma once

#include <common/helpers.h>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace auth_svc {

// Re-export from common — auth handlers use jsonError via `using namespace auth_svc`
using common::jsonError;

inline std::string genUuid() {
  static thread_local std::mt19937_64 gen{std::random_device{}()};
  uint64_t a = gen(), b = gen();
  std::stringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(8)
     << static_cast<uint32_t>(a >> 32) << "-" << std::setw(4)
     << static_cast<uint16_t>(a >> 16) << "-" << std::setw(4)
     << ((static_cast<uint16_t>(a) & 0x0fff) | 0x4000) << "-" << std::setw(4)
     << ((static_cast<uint16_t>(b >> 48) & 0x3fff) | 0x8000) << "-"
     << std::setw(12) << (b & 0xffffffffffffULL);
  return ss.str();
}

}  // namespace auth_svc
