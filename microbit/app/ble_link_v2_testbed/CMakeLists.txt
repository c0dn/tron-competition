include(${CMAKE_CURRENT_SOURCE_DIR}/../ble_mesh_node/cmake/TronBleProfiles.cmake)

tron_ble_configure_phase1_target(
  TARGET ble_link_v2_testbed
  KIND LINK
  GENERATED_DIR TRON_BLE_LINK_GENERATED_DIR
  BUILD_INFO_SOURCE TRON_BLE_LINK_BUILD_INFO_SOURCE
  SOURCES TRON_BLE_LINK_SOURCES
)

mtkernel_add_microbit_firmware(
  TARGET ble_link_v2_testbed
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../ble_mesh_node/config
  APP_SOURCES
    ${TRON_BLE_LINK_SOURCES}
)

target_include_directories(ble_link_v2_testbed PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${TRON_BLE_LINK_GENERATED_DIR}
)

# μT-Kernel supplies its own freestanding size_t.  The Phase 1 public wire
# headers also use size_t, so make the kernel definition visible before the
# toolchain stddef wrapper for every harness translation unit.
target_compile_definitions(ble_link_v2_testbed PRIVATE _SIZE_T_DEFINED _SIZE_T_DECLARED)
target_compile_options(ble_link_v2_testbed PRIVATE -include tk/tkernel.h)
