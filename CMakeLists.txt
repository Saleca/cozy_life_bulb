cmake_minimum_required(VERSION 3.15)
project(cozy_life_bulb LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -O2)
endif()

add_executable(cozy_life_bulb main.c)

target_link_libraries(cozy_life_bulb PRIVATE ws2_32)

add_definitions(-D_CRT_SECURE_NO_WARNINGS)