cmake_minimum_required(VERSION 3.24)

project(ApiGate VERSION 0.1.0 LANGUAGES CXX)

find_package(boost_beast REQUIRED CONFIG)
find_package(boost_asio REQUIRED CONFIG)
add_executable(api-gate
    src/main.cpp
)
target_link_libraries(api-gate PRIVATE Boost::beast Boost::asio)

target_compile_features(api-gate PRIVATE cxx_std_20)
set_target_properties(api-gate PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(api-gate PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(api-gate PRIVATE -Wall -Wextra -Wpedantic)
endif()

