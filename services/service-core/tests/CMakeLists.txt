add_executable(core_tests
  test_domain.cc
  test_state_machine.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/domain.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/state_machine.cc
)

target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(core_tests PRIVATE
  Catch2::Catch2WithMain
)

catch_discover_tests(core_tests)
