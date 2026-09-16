add_executable(core_tests
  test_domain.cc
  test_state_machine.cc
  test_emulator.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/domain.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/state_machine.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/emulator.cc
  # emulator.cc calls DealRepository::find/apply and toDealState, so
  # repository.cc's symbols must be linked in too, even though none of the
  # tests below exercise emulator.cc's DB-touching code paths (only its pure
  # functions) — the linker still resolves every symbol the .cc references.
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/repository.cc
)

target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(core_tests PRIVATE
  Catch2::Catch2WithMain
  common
  Drogon::Drogon
)

catch_discover_tests(core_tests)
