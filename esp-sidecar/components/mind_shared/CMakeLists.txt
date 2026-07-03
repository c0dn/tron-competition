# Interface-only component: exposes the repo-root shared/ wire contracts
# (schema.h, uplink_schema.h) as an include path to the rest of the firmware.
# Keeps the single-source-of-truth headers out of any one component.
idf_component_register(INCLUDE_DIRS "../../../shared")
