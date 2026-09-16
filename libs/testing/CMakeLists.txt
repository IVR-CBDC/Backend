# Каталог добавляется раньше libs/common, поэтому цель PkgConfig::JWT
# может ещё не существовать — заводим её здесь, как это делает common.
if(NOT TARGET PkgConfig::JWT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JWT REQUIRED IMPORTED_TARGET libjwt)
endif()

add_library(testing_support INTERFACE)
target_include_directories(testing_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(testing_support INTERFACE PkgConfig::JWT)
