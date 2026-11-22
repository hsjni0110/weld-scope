cmake_minimum_required(VERSION 3.24)
project(WeldScope LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_subdirectory(src/WeldScope.Device)
add_library(weldscope_inspection_input STATIC
    src/WeldScope.FileInspection/InspectionInput.cpp
    src/WeldScope.FileInspection/AnalysisPipeline.cpp)
target_include_directories(weldscope_inspection_input PUBLIC src/WeldScope.FileInspection)
target_include_directories(weldscope_inspection_input SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(weldscope_inspection_input PUBLIC opencv_core opencv_imgproc opencv_imgcodecs PRIVATE bcrypt)
add_executable(weldscope_inspect src/WeldScope.Cli/main.cpp src/WeldScope.Cli/InspectionFiles.cpp)
target_link_libraries(weldscope_inspect PRIVATE weldscope_inspection_input)
add_executable(analysis_pipeline_tests tests/analysis_pipeline_tests.cpp)
target_link_libraries(analysis_pipeline_tests PRIVATE weldscope_inspection_input)
add_test(NAME analysis_pipeline COMMAND analysis_pipeline_tests "${CMAKE_CURRENT_SOURCE_DIR}/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WeldScope.App/res/samples/defect.png")
add_executable(file_inspection_job_tests tests/file_inspection_job_tests.cpp src/WeldScope.App/FileInspectionJob.cpp)
target_include_directories(file_inspection_job_tests PRIVATE src/WeldScope.App)
target_link_libraries(file_inspection_job_tests PRIVATE weldscope_inspection_input)
add_test(NAME file_inspection_job COMMAND file_inspection_job_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WeldScope.App/res/samples/defect.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/pooled-smooth-220.json")
foreach(target weldscope_inspection_input weldscope_inspect analysis_pipeline_tests file_inspection_job_tests)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive-)
endforeach()

option(WELDSCOPE_S10_TESTS "Build the x64 S10 integration test host" OFF)
if(WELDSCOPE_S10_TESTS)
    add_subdirectory(src/WeldScope.Storage)
    add_library(weldscope_integrated_inspection STATIC
        src/WeldScope.App/FileInspectionJob.cpp src/WeldScope.App/IntegratedInspection.cpp
        src/WeldScope.App/InspectionDetails.cpp
        src/WeldScope.Device/LoopbackServer.cpp src/WeldScope.Device/Protocol.cpp)
    target_include_directories(weldscope_integrated_inspection PUBLIC src/WeldScope.App src/WeldScope.Device)
    target_link_libraries(weldscope_integrated_inspection PUBLIC weldscope_inspection_input weldscope_storage ws2_32 rpcrt4)
    target_compile_options(weldscope_integrated_inspection PRIVATE /W4 /WX /utf-8 /permissive-)
    add_executable(s10_integration_host tests/s10_integration_host.cpp)
    target_link_libraries(s10_integration_host PRIVATE weldscope_integrated_inspection)
    target_compile_options(s10_integration_host PRIVATE /W4 /WX /utf-8 /permissive-)
    add_dependencies(s10_integration_host weldscope_history)
    foreach(runtime libmysql.dll libssl-3-x64.dll libcrypto-3-x64.dll)
        add_custom_command(TARGET s10_integration_host POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:weldscope_history>/${runtime}" "$<TARGET_FILE_DIR:s10_integration_host>" VERBATIM)
    endforeach()
    add_executable(folder_discovery_tests tests/folder_discovery_tests.cpp)
    target_link_libraries(folder_discovery_tests PRIVATE weldscope_integrated_inspection)
    target_compile_options(folder_discovery_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    add_dependencies(folder_discovery_tests s10_integration_host)
    add_test(NAME folder_discovery COMMAND folder_discovery_tests)
endif()
