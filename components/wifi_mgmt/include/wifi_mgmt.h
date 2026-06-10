#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"

class WifiMgmt {
    void start_ap();
    void start_sta(const char *ssid, const char *password);
    bool get_sta_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

    bool is_ap_mode = false;
    char ip_str[16] = "0.0.0.0";
    int retry_count = 0;
    EventGroupHandle_t wifi_event_group{};

    esp_event_handler_instance_t wifi_ev_instance{};
    esp_event_handler_instance_t ip_ev_instance{};

public:
    ~WifiMgmt();
    void start();

    /* Get current Wi-Fi mode info */
    bool ap_mode();
    const char *get_ip();

    /* Configure STA credentials (saves to NVS, restarts Wi-Fi in STA mode) */
    void set_sta_credentials(const char *ssid, const char *password);

    /* Force AP mode (e.g., via boot button) */
    void force_ap();

    void ev_handler(esp_event_base_t event_base, int32_t event_id, void* event_data);
};