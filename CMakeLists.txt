cmake_minimum_required(VERSION 3.15)
project(cozy_life_bulb LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_library(MOSQUITTO_LIB mosquitto)
find_library(CJSON_LIB cjson)

add_executable(cozy_life_bulb main.c)

target_link_libraries(cozy_life_bulb PRIVATE 
    ${MOSQUITTO_LIB}
    ${CJSON_LIB}
)