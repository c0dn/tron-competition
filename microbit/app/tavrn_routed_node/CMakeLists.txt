include(${CMAKE_CURRENT_SOURCE_DIR}/../ble_mesh_node/cmake/TronBleProfiles.cmake)

tron_ble_configure_phase1_target(
  TARGET tavrn_routed_node
  KIND ROUTED
  GENERATED_DIR TRON_BLE_ROUTED_GENERATED_DIR
  BUILD_INFO_SOURCE TRON_BLE_ROUTED_BUILD_INFO_SOURCE
  SOURCES TRON_BLE_ROUTED_SOURCES
)

mtkernel_add_microbit_firmware(
  TARGET tavrn_routed_node
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../ble_mesh_node/config
  APP_SOURCES
    ${TRON_BLE_ROUTED_SOURCES}
)

target_include_directories(tavrn_routed_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${TRON_BLE_ROUTED_GENERATED_DIR}
)

target_compile_definitions(tavrn_routed_node PRIVATE _SIZE_T_DEFINED _SIZE_T_DECLARED)
target_compile_options(tavrn_routed_node PRIVATE -include tk/tkernel.h)
