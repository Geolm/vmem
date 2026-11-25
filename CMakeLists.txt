cmake_minimum_required(VERSION 3.13)
project(vmem C)

add_executable(unit_tests tests/test_vmem.c)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Same tests under AddressSanitizer: exercises the poison/unpoison paths.
add_executable(unit_tests_asan tests/test_vmem.c)
target_include_directories(unit_tests_asan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(unit_tests_asan PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(unit_tests_asan PRIVATE -fsanitize=address)
