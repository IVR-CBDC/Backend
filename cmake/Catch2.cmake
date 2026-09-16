# Catch2 подтягивается один раз на весь workspace: и libs/common/tests, и
# services/*/tests линкуются к одной и той же цели Catch2::Catch2WithMain.
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
