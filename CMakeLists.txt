cmake_minimum_required(VERSION 3.10)
project(slaptrack VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(slaptrack
    src/main.cpp
    src/log_parser.cpp
    src/viewer.cpp
    src/filter.cpp
)

target_include_directories(slaptrack PRIVATE src)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(slaptrack PRIVATE -Wall -Wextra -g)
else()
    target_compile_options(slaptrack PRIVATE -O2)
endif()
