cmake_minimum_required(VERSION 3.24)
project(ApiGate VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ApiGate only supports Linux.")
endif()

include(CTest)
include(GNUInstallDirs)
option(APIGATE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(APIGATE_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)
option(APIGATE_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

function(apigate_set_project_options target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion -Wformat=2)
    if(APIGATE_WARNINGS_AS_ERRORS)
        target_compile_options(${target_name} PRIVATE -Werror)
    endif()
    if(APIGATE_ENABLE_SANITIZERS)
        target_compile_options(${target_name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
    endif()
    if(APIGATE_ENABLE_CLANG_TIDY)
        find_program(APIGATE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
        set_target_properties(${target_name} PROPERTIES CXX_CLANG_TIDY
            "${APIGATE_CLANG_TIDY_EXECUTABLE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
    endif()
endfunction()

add_library(apigate_core
    src/application.cpp
    src/config.cpp
    src/http_handler.cpp
    src/http_server.cpp
    src/logging.cpp)
target_include_directories(apigate_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(apigate_core
    PUBLIC Boost::beast nlohmann_json::nlohmann_json
    PRIVATE Boost::asio spdlog::spdlog)
apigate_set_project_options(apigate_core)

add_executable(api-gate src/main.cpp)
target_link_libraries(api-gate PRIVATE apigate_core)
set_target_properties(api-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
apigate_set_project_options(api-gate)
install(TARGETS api-gate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(apigate_unit_tests
        tests/unit/config_test.cpp
        tests/unit/http_handler_test.cpp
        tests/unit/logging_test.cpp)
    target_link_libraries(apigate_unit_tests PRIVATE apigate_core GTest::gtest_main)
    apigate_set_project_options(apigate_unit_tests)
    include(GoogleTest)
    gtest_discover_tests(apigate_unit_tests)
    add_test(NAME integration.config_check COMMAND api-gate --check-config)
    add_test(NAME integration.service_lifecycle
        COMMAND bash ${PROJECT_SOURCE_DIR}/tests/integration/service_lifecycle.sh
                     $<TARGET_FILE:api-gate>)
    add_test(NAME integration.service_lifecycle_sigint
        COMMAND bash ${PROJECT_SOURCE_DIR}/tests/integration/service_lifecycle.sh
                     $<TARGET_FILE:api-gate> INT)
    add_test(NAME integration.install_deps_safety
        COMMAND bash ${PROJECT_SOURCE_DIR}/tests/integration/install_deps_safety.sh)
    add_test(NAME integration.container_smoke_safety
        COMMAND bash ${PROJECT_SOURCE_DIR}/tests/integration/container_smoke_safety.sh)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME integration.http_server
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/http_server_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(
        integration.config_check
        integration.service_lifecycle
        integration.service_lifecycle_sigint
        integration.install_deps_safety
        integration.container_smoke_safety
        integration.http_server
        PROPERTIES TIMEOUT 15)
    set_tests_properties(integration.http_server PROPERTIES TIMEOUT 40)
    set_tests_properties(integration.install_deps_safety PROPERTIES SKIP_RETURN_CODE 77)
endif()
