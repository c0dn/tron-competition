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
    ${CMAKE_CURRENT_SOURCE_DIR}/../mesh/src/mesh_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/hw_timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/imu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/mic.c
)

# mesh_flood.c and hw_timer.c stay in the source list whatever MESH_ENABLE is,
# so toggling the mesh is a one-line change in app_config.h.
#
# Note this build has no -ffunction-sections/--gc-sections, so those objects are
# linked either way: MESH_ENABLE=0 measured 62300 bytes of text against 62352
# with the mesh on, a 52-byte difference. The switch buys BEHAVIOUR (no relay
# task, no transport header on air), not flash. If flash ever becomes the
# binding constraint, drop the two sources from this list rather than expecting
# the preprocessor to have done it.
target_include_directories(wearable_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../mesh/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
)
