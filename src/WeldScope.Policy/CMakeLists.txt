cmake_minimum_required(VERSION 3.24)
project(WeldScopePolicy LANGUAGES CXX)

add_library(weldscope_policy STATIC ../WeldScope.Core/SelectiveInspectionPolicy.cpp)
target_include_directories(weldscope_policy PUBLIC ../WeldScope.Core)
target_compile_features(weldscope_policy PUBLIC cxx_std_17)

add_executable(weldscope_policy_demo ../WeldScope.Cli/SelectiveInspectionDemo.cpp)
target_link_libraries(weldscope_policy_demo PRIVATE weldscope_policy)
set_target_properties(weldscope_policy_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

enable_testing()
add_executable(selective_inspection_policy_tests ../../tests/selective_inspection_policy_tests.cpp)
target_link_libraries(selective_inspection_policy_tests PRIVATE weldscope_policy)
add_test(NAME selective_inspection_policy COMMAND selective_inspection_policy_tests)
add_test(NAME selective_inspection_demo COMMAND "${CMAKE_COMMAND}"
    "-DDEMO=$<TARGET_FILE:weldscope_policy_demo>"
    "-DEXPECTED_OUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/../../docs/steps/S08-evidence/sample-console.txt"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/check_selective_inspection_demo.cmake")

if(MSVC)
    foreach(target weldscope_policy weldscope_policy_demo selective_inspection_policy_tests)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive-)
    endforeach()
endif()
