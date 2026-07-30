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
)

# microbit_mesh stays linked whatever MESH_ENABLE is, so toggling the mesh is a
# one-line change in app_config.h.
#
# Note this build has no -ffunction-sections/--gc-sections, so its propagated
# sources are linked either way: MESH_ENABLE=0 measured 62300 bytes of text
# against 62352 with the mesh on, a 52-byte difference. The switch buys
# BEHAVIOUR (no relay task, no transport header on air), not flash. If flash
# becomes the binding constraint, stop linking microbit_mesh for that target
# rather than expecting the preprocessor to have removed it.
target_include_directories(wearable_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(wearable_app PRIVATE
  microbit_mesh
  microbit_driver_imu
  microbit_driver_mic
)
