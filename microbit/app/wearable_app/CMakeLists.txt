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
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/imu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/mic.c
)

target_include_directories(wearable_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
)

# Only main.c receives the force-included hooks. Keeping the production
# implementation units untouched prevents function-definition substitutions.
add_library(wearable_test_injector_main OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)
target_link_libraries(wearable_test_injector_main PRIVATE mtkernel3_microbit_common)
target_include_directories(wearable_test_injector_main PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/config
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/mtkernel_3/config
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared
)
target_compile_definitions(wearable_test_injector_main PRIVATE WEARABLE_TEST_INJECTOR=1)
target_compile_options(wearable_test_injector_main PRIVATE
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/wearable_test_injector.h"
)

set(wearable_test_injector_sources
  $<TARGET_OBJECTS:wearable_test_injector_main>
  ${CMAKE_CURRENT_SOURCE_DIR}/src/imath.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sensors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fall.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sound.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fusion.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_emit.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tx_adapter.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/wearable_test_injector.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tron_mesh_packet.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/imu.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/mic.c
)

mtkernel_add_microbit_firmware(
  TARGET wearable_test_injector
  CONFIG_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  APP_SOURCES
    ${wearable_test_injector_sources}
)

target_include_directories(wearable_test_injector PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
)
target_compile_definitions(wearable_test_injector PRIVATE WEARABLE_TEST_INJECTOR=1)
