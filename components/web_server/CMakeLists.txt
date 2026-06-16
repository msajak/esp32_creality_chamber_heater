idf_component_register(
    SRCS "web_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer wifi_mgmt ota shared logger
    EMBED_FILES "web_ui/index.html"
)
