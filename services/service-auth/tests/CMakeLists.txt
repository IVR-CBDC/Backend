add_executable(auth_tests
  test_jwt_issuer.cc
  test_validation.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/jwt_issuer.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/validation.cc
)

target_include_directories(auth_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(auth_tests PRIVATE
  common
  testing_support
  Catch2::Catch2WithMain
)

# PROPERTIES ENVIRONMENT "A=1;B=2" (даже с $<SEMICOLON>) ломается внутри
# catch_discover_tests: значение переразбирается на плоский список аргументов
# на пути от нашего вызова до CatchAddTests.cmake, и set_tests_properties
# получает нечётное число позиционных аргументов вместо одного списка —
# JWT_PRIVATE_KEY_PATH молча теряется. Ставим оба env-var через
# TEST_INCLUDE_FILES, как советует сам Catch.cmake для «more fine-grained
# test control»: там ${<target>_TESTS} — обычная CMake-переменная в области
# видимости включаемого скрипта, и один вызов set_tests_properties с
# quoted-списком работает как положено.
catch_discover_tests(auth_tests
  TEST_LIST auth_tests_TESTS
)

set(auth_tests_env_script "${CMAKE_CURRENT_BINARY_DIR}/auth_tests_env.cmake")
file(WRITE "${auth_tests_env_script}"
  "set_tests_properties(\${auth_tests_TESTS} PROPERTIES ENVIRONMENT \"JWT_PUBLIC_KEY_PATH=${TEST_KEYS_A}/jwt_public.jwk;JWT_PRIVATE_KEY_PATH=${TEST_KEYS_A}/jwt_private.jwk\")\n"
)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${auth_tests_env_script}")
