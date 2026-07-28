cmake_minimum_required(VERSION 3.20)

project(LogBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(logbridge
        src/main.cpp
)

target_compile_options(logbridge PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)