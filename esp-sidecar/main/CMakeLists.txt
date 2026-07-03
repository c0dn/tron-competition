idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES mind_ble mind_uplink mind_shared
                                nvs_flash esp_netif esp_event)
