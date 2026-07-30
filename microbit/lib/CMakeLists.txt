# First-party reusable micro:bit modules.
#
# Keep these separate from:
#   app/  - firmware entry points and app-specific configuration
#   libs/ - vendored third-party code (micro T-Kernel)
#
# INTERFACE targets are intentional. Their sources are compiled as part of each
# consuming firmware target, so they inherit that app's config/config.h and
# compile definitions instead of being compiled once against a global kernel
# configuration.

add_library(mind_wire_contract INTERFACE)
target_include_directories(mind_wire_contract INTERFACE
  ${PROJECT_SOURCE_DIR}/../shared
)

add_subdirectory(drivers)
add_subdirectory(mesh)
