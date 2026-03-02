cmake_minimum_required(VERSION 3.15)
project(cozy_life_bulb LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(MOSQUITTO_DIR "C:/Program Files/mosquitto")

add_executable(cozy_life_bulb
    main.c
    "cjson/cJSON.c")


target_include_directories(cozy_life_bulb PRIVATE
    "."
    "${MOSQUITTO_DIR}/devel"
)

target_link_libraries(cozy_life_bulb PRIVATE
    "${MOSQUITTO_DIR}/devel/mosquitto.lib"
    ws2_32)

target_compile_definitions(cozy_life_bulb PRIVATE _CRT_SECURE_NO_WARNINGS)

# windows
add_custom_command(TARGET cozy_life_bulb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${MOSQUITTO_DIR}/mosquitto.dll"
    "${MOSQUITTO_DIR}/cjson.dll"
    "${MOSQUITTO_DIR}/mosquitto_common.dll"
    "${MOSQUITTO_DIR}/libssl-3-x64.dll"
    "${MOSQUITTO_DIR}/libcrypto-3-x64.dll"
    "${MOSQUITTO_DIR}/pthreadVC3.dll"
    "$<TARGET_FILE_DIR:cozy_life_bulb>")