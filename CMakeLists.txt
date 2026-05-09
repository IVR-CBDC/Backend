cmake_minimum_required(VERSION 3.20)
project(ivr_workspace)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(libs/common)
add_subdirectory(services/service-auth)
add_subdirectory(services/service-core)

