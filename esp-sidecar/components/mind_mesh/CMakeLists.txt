idf_component_register(SRCS "mind_mesh.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif mind_shared mesh_lite iot_bridge)
