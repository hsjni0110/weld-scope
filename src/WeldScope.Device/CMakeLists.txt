cmake_minimum_required(VERSION 3.24)
project(WeldScopeDevice LANGUAGES CXX)

add_library(weldscope_device_protocol STATIC Protocol.cpp)
target_compile_features(weldscope_device_protocol PUBLIC cxx_std_17)

enable_testing()
add_executable(weldscope_device_protocol_tests ProtocolTests.cpp)
target_link_libraries(weldscope_device_protocol_tests PRIVATE weldscope_device_protocol)
add_test(NAME s06_protocol COMMAND weldscope_device_protocol_tests)

if(MSVC)
    foreach(target weldscope_device_protocol weldscope_device_protocol_tests)
        target_compile_options(${target} PRIVATE /W4 /WX /EHsc /utf-8 /permissive-)
    endforeach()
endif()
