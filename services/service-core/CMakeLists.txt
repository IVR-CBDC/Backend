cmake_minimum_required(VERSION 3.20)
project(service_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Wall -Wextra -Wpedantic -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)

# For standalone builds (Docker), add common if not already present
if(NOT TARGET common)
  set(COMMON_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/common"
      CACHE PATH "Path to libs/common")
  add_subdirectory(${COMMON_LIB_DIR} ${CMAKE_CURRENT_BINARY_DIR}/_common)
endif()

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS src/*.cc)

add_executable(service_core ${SRC})

target_include_directories(service_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(service_core PRIVATE
  common
  Drogon::Drogon
  OpenSSL::SSL OpenSSL::Crypto
  PkgConfig::JWT
)
