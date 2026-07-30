mtkernel_add_microbit_firmware(
  TARGET mesh_echo
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)

target_include_directories(mesh_echo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mesh_echo PRIVATE
  microbit_driver_ble_radio
  microbit_driver_hw_timer
  mind_wire_contract
)
