# Host unit tests for the VR bridge, buildable on their own (no ROM, no recompiled code):
#   cmake -S src/vr/tests -B build-vrtest && cmake --build build-vrtest && build-vrtest/dk64vr_tests
cmake_minimum_required(VERSION 3.20)
project(dk64vr_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

set(DK64VR_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
add_executable(dk64vr_tests
    ${CMAKE_CURRENT_LIST_DIR}/dk64vr_tests.cpp
    ${DK64VR_ROOT}/src/vr/bridge/dk64vr_math.cpp
    ${DK64VR_ROOT}/src/vr/bridge/dk64vr_sched.cpp
    ${DK64VR_ROOT}/src/vr/bridge/dk64vr_settings.c
    ${DK64VR_ROOT}/src/vr/bridge/dk64vr_log.c
    ${DK64VR_ROOT}/third_party/sfvr/src/sfvr_settings.c)
target_include_directories(dk64vr_tests PRIVATE ${DK64VR_ROOT}/src/vr/bridge ${DK64VR_ROOT}/third_party/sfvr/include)
target_compile_options(dk64vr_tests PRIVATE -Wall -Wextra -ffp-contract=off)
target_link_libraries(dk64vr_tests PRIVATE m)
enable_testing()
add_test(NAME dk64vr_tests COMMAND dk64vr_tests)
