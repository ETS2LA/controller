cmake_minimum_required(VERSION 3.15)
project(controller_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(controller_example main.cpp)
target_include_directories(controller_example PRIVATE ../../include)