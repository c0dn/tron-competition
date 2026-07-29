set(RF_MESH_NODE_ID "0" CACHE STRING "rf_mesh_node explicit node id; 0 derives a non-root id from FICR")
set(RF_MESH_IS_ROOT "0" CACHE STRING "rf_mesh_node role: 1=root, 0=normal node")
set(RF_MESH_NETWORK_ID "0x5452" CACHE STRING "rf_mesh_node v1 network id")
set(RF_MESH_ROOT_ID "0x0001" CACHE STRING "rf_mesh_node configured root id")
set(RF_MESH_RF_CHANNEL "7" CACHE STRING "rf_mesh_node RF channel, 0..83 => 2400 MHz + channel")
set(RF_MESH_RF_TXPOWER "0x00" CACHE STRING "rf_mesh_node raw nRF RADIO TXPOWER field value")

mtkernel_add_microbit_firmware(
  TARGET rf_mesh_node
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../rf_mesh_core/rf_mesh_packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../rf_mesh_core/rf_mesh_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/rf_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/display.c
)

target_compile_definitions(rf_mesh_node PRIVATE
  RF_MESH_NODE_ID=${RF_MESH_NODE_ID}
  RF_MESH_IS_ROOT=${RF_MESH_IS_ROOT}
  RF_MESH_NETWORK_ID=${RF_MESH_NETWORK_ID}
  RF_MESH_ROOT_ID=${RF_MESH_ROOT_ID}
  RF_MESH_RF_CHANNEL=${RF_MESH_RF_CHANNEL}
  RF_MESH_RF_TXPOWER=${RF_MESH_RF_TXPOWER}
)

target_include_directories(rf_mesh_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../rf_mesh_core
)
