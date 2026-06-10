idf_component_register(
    SRCS "wifi_mgmt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif nvs_flash esp_event
)
