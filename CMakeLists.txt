cmake_minimum_required(VERSION 3.20)
project(ivr_workspace)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Тесты собираются только в workspace-сборке: Dockerfile каждого сервиса
# конфигурирует его CMakeLists напрямую, и там BUILD_TESTS не определён.
option(BUILD_TESTS "Build C++ unit tests" ON)

if(BUILD_TESTS)
  enable_testing()
  include(cmake/Catch2.cmake)
  add_subdirectory(libs/testing)

  # Две независимые пары ключей: A — «своя», B — чужая подпись.
  set(TEST_KEYS_A ${CMAKE_BINARY_DIR}/test-keys-a)
  set(TEST_KEYS_B ${CMAKE_BINARY_DIR}/test-keys-b)
  execute_process(COMMAND bash ${CMAKE_SOURCE_DIR}/infra/gen-keys.sh ${TEST_KEYS_A}
                  RESULT_VARIABLE keys_a_rc OUTPUT_QUIET)
  execute_process(COMMAND bash ${CMAKE_SOURCE_DIR}/infra/gen-keys.sh ${TEST_KEYS_B}
                  RESULT_VARIABLE keys_b_rc OUTPUT_QUIET)
  if(NOT keys_a_rc EQUAL 0 OR NOT keys_b_rc EQUAL 0)
    message(FATAL_ERROR "не удалось сгенерировать тестовые ключи (нужны openssl и python3 с cryptography)")
  endif()
endif()

add_subdirectory(libs/common)
add_subdirectory(services/service-auth)
add_subdirectory(services/service-core)
