# First-party reusable micro:bit modules.
#
# Keep these separate from:
#   app/  - firmware entry points and app-specific configuration
#   libs/ - vendored third-party code (micro T-Kernel)
#
# INTERFACE targets are intentional. Their sources are compiled as part of each
# consuming firmware target, so they inherit that app's config/config.h and
# compile definitions instead of being compiled once against a global kernel
# configuration. That matters here: CNF_TIMER_PERIOD is 1 ms in ble_mesh_node
# and ble_observer but 10 ms in wearable_app and ble_beacon, so a single shared
# object build of the same driver would be wrong for half the firmwares.

add_library(mind_wire_contract INTERFACE)
target_include_directories(mind_wire_contract INTERFACE
  ${PROJECT_SOURCE_DIR}/../shared
)

add_subdirectory(drivers)
