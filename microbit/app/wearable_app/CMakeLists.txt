mtkernel_add_microbit_firmware(
  TARGET wearable_app
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imath.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sensors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sound.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fusion.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_emit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/imu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/mic.c
)

target_include_directories(wearable_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
)
