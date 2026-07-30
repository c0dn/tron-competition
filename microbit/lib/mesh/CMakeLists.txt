# Controlled BLE flood mesh. This is a reusable transport module, not a
# firmware application: apps provide an entry point and call mesh_init(),
# mesh_originate(), and mesh_task().

add_library(microbit_mesh INTERFACE)
target_sources(microbit_mesh INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/mesh_flood.c
)
target_include_directories(microbit_mesh INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(microbit_mesh INTERFACE
  microbit_driver_ble_radio
  microbit_driver_hw_timer
  mind_wire_contract
)
