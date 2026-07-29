# shared/ carries schema.h, the single source of truth for the BLE payload that
# the micro:bit encodes against. Included by path rather than copied so the
# sniffer cannot drift from the contract.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "../../../../shared"
                       REQUIRES bt nvs_flash esp_timer)
