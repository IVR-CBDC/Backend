add_executable(common_tests test_jwt_verifier.cc)

target_link_libraries(common_tests PRIVATE
  common
  testing_support
  Catch2::Catch2WithMain
)

catch_discover_tests(common_tests
  PROPERTIES ENVIRONMENT "JWT_PUBLIC_KEY_PATH=${TEST_KEYS_A}/jwt_public.jwk"
)
target_compile_definitions(common_tests PRIVATE
  TEST_KEYS_A="${TEST_KEYS_A}"
  TEST_KEYS_B="${TEST_KEYS_B}"
)
