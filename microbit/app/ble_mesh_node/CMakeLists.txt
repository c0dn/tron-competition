include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/TronBleProfiles.cmake)

tron_ble_configure_phase1_target(
  TARGET ble_mesh_node
  KIND LEGACY
  GENERATED_DIR TRON_BLE_MESH_NODE_GENERATED_DIR
  BUILD_INFO_SOURCE TRON_BLE_MESH_NODE_BUILD_INFO_SOURCE
  SOURCES TRON_BLE_MESH_NODE_SOURCES
)

mtkernel_add_microbit_firmware(
  TARGET ble_mesh_node
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${TRON_BLE_MESH_NODE_SOURCES}
)

target_compile_definitions(ble_mesh_node PRIVATE
  TRON_NODE_ID=${TRON_NODE_ID}
  TRON_NODE_BLOCK_DIRECT_PEER_ID=${TRON_TEST_RX_BLOCK_PEER_ID_EFFECTIVE}
)

target_include_directories(ble_mesh_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${TRON_BLE_MESH_NODE_GENERATED_DIR}
)
