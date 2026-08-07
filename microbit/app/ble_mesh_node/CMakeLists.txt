set(TRON_NODE_ID "0" CACHE STRING "ble_mesh_node explicit node id; 0 derives the id from FICR")
set(TRON_NODE_BLOCK_DIRECT_PEER_ID "0" CACHE STRING "test-only originator id whose full-TTL (direct) frames are ignored; 0 disables")

mtkernel_add_microbit_firmware(
  TARGET ble_mesh_node
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tron_mesh_packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tron_mesh_dedupe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/ble_mesh_scheduler.c
)

target_compile_definitions(ble_mesh_node PRIVATE
  TRON_NODE_ID=${TRON_NODE_ID}
  TRON_NODE_BLOCK_DIRECT_PEER_ID=${TRON_NODE_BLOCK_DIRECT_PEER_ID}
)

target_link_libraries(ble_mesh_node PRIVATE
  microbit_driver_ble_radio
  mind_wire_contract
)

target_include_directories(ble_mesh_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)
