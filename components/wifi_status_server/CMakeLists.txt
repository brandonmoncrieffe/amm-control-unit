idf_component_register(
    SRCS "wifi_status_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES servo_control audio_processing esp_wifi esp_netif esp_event
                  nvs_flash esp_http_server
)
