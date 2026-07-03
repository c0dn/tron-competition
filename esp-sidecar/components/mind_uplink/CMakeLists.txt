idf_component_register(SRCS "mind_uplink.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt esp_timer esp_event
                                mind_ble mind_mesh mind_shared)
