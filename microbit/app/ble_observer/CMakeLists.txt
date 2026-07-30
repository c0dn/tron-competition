mtkernel_add_microbit_firmware(
  TARGET ble_observer
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)

target_include_directories(ble_observer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ble_observer PRIVATE
  microbit_driver_ble_radio
  microbit_driver_display
)
