idf_component_register(
    SRCS "summon_voice.c" "summon_network.c"
         "provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES json mbedtls bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash esp_websocket_client lwip
)
