mtkernel_add_microbit_firmware(
  TARGET test_firmware
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app_main.c
)
