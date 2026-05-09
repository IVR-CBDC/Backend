cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET Drogon::Drogon)
  find_package(Drogon CONFIG REQUIRED)
endif()
if(NOT TARGET PkgConfig::JWT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)
endif()

add_library(common OBJECT
  src/jwt_verifier.cc
  src/jwt_filter.cc
)

target_include_directories(common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(common PUBLIC
  Drogon::Drogon
  PkgConfig::JWT
)
