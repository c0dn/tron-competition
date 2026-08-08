set(WEARABLE_TEST_INJECT "0" CACHE STRING "bench only: periodically inject synthetic incidents to exercise the transmit path; 0 disables")
set(WEARABLE_TEST_INJECT_PERIOD_MS "8000" CACHE STRING "period between injected test incidents, ms")

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tx_adapter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tron_mesh_packet.c
)

target_link_libraries(wearable_app PRIVATE
  microbit_driver_ble_radio
  microbit_driver_imu
  microbit_driver_mic
  mind_wire_contract
)

target_compile_definitions(wearable_app PRIVATE
  WEARABLE_TEST_INJECT=${WEARABLE_TEST_INJECT}
  WEARABLE_TEST_INJECT_PERIOD_MS=${WEARABLE_TEST_INJECT_PERIOD_MS}
)

target_include_directories(wearable_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)
