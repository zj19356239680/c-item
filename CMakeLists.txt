cmake_minimum_required(VERSION 3.24)
include(cmake/vcpkg-cache.cmake)
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
find_package(OpenSSL REQUIRED)

set(APIGATE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(
    src/version.hpp.in
    "${APIGATE_GENERATED_INCLUDE_DIR}/apigate/version.hpp"
    @ONLY)

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
    src/buffer_budget.cpp
    src/config.cpp
    src/api_key_auth.cpp
    src/downstream_tls.cpp
    src/downstream_transport.cpp
    src/health_check.cpp
    src/metrics.cpp
    src/health_state.cpp
    src/http_handler.cpp
    src/upstream_pool.cpp
    src/http_proxy.cpp
    src/http_server.cpp
    src/rate_limit.cpp
    src/route_proxy_admission.cpp
    src/upstream_tls.cpp
    src/logging.cpp)
target_include_directories(apigate_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(apigate_core
    PUBLIC Boost::beast nlohmann_json::nlohmann_json
    PRIVATE Boost::asio spdlog::spdlog OpenSSL::SSL OpenSSL::Crypto)
apigate_set_project_options(apigate_core)

add_executable(api-gate src/main.cpp)
target_include_directories(api-gate PRIVATE "${APIGATE_GENERATED_INCLUDE_DIR}")
target_link_libraries(api-gate PRIVATE apigate_core)
set_target_properties(api-gate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
apigate_set_project_options(api-gate)
install(TARGETS api-gate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(apigate_unit_tests
        tests/unit/buffer_budget_test.cpp
        tests/unit/config_test.cpp
        tests/unit/api_key_auth_test.cpp
        tests/unit/metrics_test.cpp
        tests/unit/health_check_test.cpp
        tests/unit/http_handler_test.cpp
        tests/unit/upstream_pool_test.cpp
        tests/unit/http_proxy_test.cpp
        tests/unit/http_server_test.cpp
        tests/unit/rate_limit_test.cpp
        tests/unit/route_proxy_admission_test.cpp
        tests/unit/logging_test.cpp)
    target_include_directories(apigate_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(apigate_unit_tests PRIVATE apigate_core GTest::gtest_main)
    apigate_set_project_options(apigate_unit_tests)
    include(GoogleTest)
    gtest_discover_tests(apigate_unit_tests)
    # Compile a separate copy with test-only access to real HttpSession state.
    # apigate_core and the production executable do not enable this seam.
    add_executable(apigate_session_write_tests
        tests/unit/http_session_write_test.cpp src/http_server.cpp)
    target_compile_definitions(apigate_session_write_tests PRIVATE APIGATE_HTTP_SESSION_TESTING)
    target_include_directories(apigate_session_write_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(apigate_session_write_tests PRIVATE apigate_core GTest::gtest_main)
    apigate_set_project_options(apigate_session_write_tests)
    gtest_discover_tests(apigate_session_write_tests PROPERTIES TIMEOUT 20)
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
    add_test(NAME integration.build_parallelism
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/integration/build_parallelism_test.py)
    set_tests_properties(integration.build_parallelism PROPERTIES TIMEOUT 15)
    add_test(NAME integration.api_key_auth
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/api_key_auth_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.api_key_auth PROPERTIES TIMEOUT 40)
    add_test(NAME integration.downstream_tls
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/downstream_tls_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.downstream_tls PROPERTIES TIMEOUT 60)
    add_test(NAME integration.http_server
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/http_server_test.py
                $<TARGET_FILE:api-gate> ${PROJECT_VERSION})
    add_test(NAME integration.header_timeout
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/header_timeout_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.header_timeout PROPERTIES TIMEOUT 45)
    add_test(NAME integration.body_timeout
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/body_timeout_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.body_timeout PROPERTIES TIMEOUT 45)
    add_test(NAME integration.upstream_pool
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/upstream_pool_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.upstream_pool PROPERTIES TIMEOUT 60)
    add_test(NAME integration.http_proxy
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/http_proxy_test.py
                $<TARGET_FILE:api-gate>)
    add_test(NAME integration.http_routes
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/http_routes_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.http_routes PROPERTIES TIMEOUT 20)
    add_test(NAME integration.rate_limit
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/rate_limit_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.rate_limit PROPERTIES TIMEOUT 20)
    add_test(NAME integration.load_balance
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/load_balance_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.load_balance PROPERTIES TIMEOUT 30)
    add_test(NAME integration.https_proxy
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/https_proxy_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.https_proxy PROPERTIES TIMEOUT 60)
    add_test(NAME integration.metrics
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/metrics_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.metrics PROPERTIES TIMEOUT 60)
    add_test(NAME integration.health_check
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/health_check_test.py
                $<TARGET_FILE:api-gate>)
    set_tests_properties(integration.health_check PROPERTIES TIMEOUT 60)
    add_test(NAME integration.capacity_baseline_smoke
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/capacity-baseline.py
                $<TARGET_FILE:api-gate> --smoke)
    set_tests_properties(
        integration.config_check
        integration.service_lifecycle
        integration.service_lifecycle_sigint
        integration.install_deps_safety
        integration.container_smoke_safety
        integration.http_server
        integration.http_proxy
        integration.capacity_baseline_smoke
        PROPERTIES TIMEOUT 15)
    set_tests_properties(integration.http_server PROPERTIES TIMEOUT 40)
    set_tests_properties(integration.http_proxy PROPERTIES TIMEOUT 30)
    set_tests_properties(integration.capacity_baseline_smoke PROPERTIES TIMEOUT 30)
    set_tests_properties(integration.install_deps_safety PROPERTIES SKIP_RETURN_CODE 77)
endif()
