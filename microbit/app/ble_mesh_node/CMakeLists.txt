mtkernel_add_microbit_firmware(
  TARGET ble_mesh_node
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tron_mesh_packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/ble_mesh_scheduler.c
)

target_include_directories(ble_mesh_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)
