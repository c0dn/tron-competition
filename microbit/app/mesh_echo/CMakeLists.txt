mtkernel_add_microbit_firmware(
  TARGET mesh_echo
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/hw_timer.c
)

target_include_directories(mesh_echo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
)
