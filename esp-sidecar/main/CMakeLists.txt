idf_component_register(SRCS "hello_world_main.c"
                       INCLUDE_DIRS "")

# TODO(decoder): to consume the shared BLE contract (<repo>/shared/schema.h),
# add it to INCLUDE_DIRS above, e.g.:
#   INCLUDE_DIRS "." "../../shared"
# Then #include "schema.h" in the decoder. See hello_world_main.c TODO.
