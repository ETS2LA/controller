cmake_minimum_required(VERSION 3.15)
project(ets2la_controller VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ets2la_controller SHARED
    src/dll/dllmain.cpp
    src/dll/input_device.cpp
)

target_include_directories(ets2la_controller PRIVATE include)
set_target_properties(ets2la_controller PROPERTIES OUTPUT_NAME "ets2la_controller")