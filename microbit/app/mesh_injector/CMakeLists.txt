# mesh_injector builds three ways from one source tree.
#
# Role and identity are compile-time (they select which tasks exist at all), and
# a bench test needs two boards behaving differently at the same time. Building
# variants here keeps that out of build.sh, so no shared script has to learn
# about -D pass-through and no one has to remember to pass it.
#
#   mesh_injector          both roles, id 0xA0 - single-board smoke test
#   mesh_injector_source   emits only,  id 0xA1 - pair with _relay
#   mesh_injector_relay    relays only, id 0xA2

function(add_mesh_injector_variant name mode devid txpower)
  mtkernel_add_microbit_firmware(
    TARGET ${name}
    CONFIG_DIR
      ${CMAKE_CURRENT_SOURCE_DIR}/config
    APP_SOURCES
      ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../mesh/src/mesh_flood.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/ble_radio.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/hw_timer.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../drivers/display.c
  )

  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../mesh/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared   # shared BLE beacon contract (schema.h)
  )

  target_compile_definitions(${name} PRIVATE
    INJ_MODE=${mode}
    INJ_DEVICE_ID=${devid}
    INJ_TXPOWER_DBM=${txpower}
  )
endfunction()

# 127 is the sentinel for "leave the radio's default power alone".
add_mesh_injector_variant(mesh_injector        INJ_MODE_BOTH   0xA0 127)
add_mesh_injector_variant(mesh_injector_source INJ_MODE_SOURCE 0xA1 127)
add_mesh_injector_variant(mesh_injector_relay  INJ_MODE_RELAY  0xA2 127)

# Range-extension test source. At -40 dBm this node is meant to be inaudible to
# a receiver that the full-power relay can still reach, which is the only way to
# demonstrate the mesh doing its actual job without a long corridor. Whether it
# works depends on physical separation: with every board on one desk the
# receiver may still hear it directly, and the test is then inconclusive rather
# than passed. Check for the absence of "direct" lines, not the presence of
# "via" ones.
add_mesh_injector_variant(mesh_injector_source_lp INJ_MODE_SOURCE 0xA1 -40)
