cmake_minimum_required(VERSION 3.10)
project(slaptrack VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# No ncurses: the ANSI edition only needs zlib/bzip2/xz for compressed logs.
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(Threads REQUIRED)

add_executable(slaptrack
    src/main.cpp
    src/viewer.cpp
    src/log_parser.cpp
    src/log_buffer.cpp
    src/log_index.cpp
    src/filter.cpp
    src/follow_tail.cpp
    src/compressed_io.cpp
    src/theme.cpp
    src/utf8.cpp
    src/widgets.cpp
    src/input.cpp
)

target_include_directories(slaptrack PRIVATE src)
target_link_libraries(slaptrack PRIVATE
    ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(slaptrack PRIVATE -Wall -Wextra -g)
else()
    target_compile_options(slaptrack PRIVATE -O2)
endif()