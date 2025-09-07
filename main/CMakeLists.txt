idf_component_register(
SRCS "main.c"
INCLUDE_DIRS "."
REQUIRES esp_http_server esp_timer esp_netif nvs_flash wifi_provisioning esp_wifi esp32-camera
)