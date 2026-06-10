#include "wifi_mgmt.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

static constexpr const char* TAG = "WIFI";

static constexpr const char* NVS_NAMESPACE = "esp32_cre_htr"; // NVS namespace must be <= 15 characters

static constexpr EventBits_t WIFI_CONNECTED_BIT{BIT0};
static constexpr EventBits_t WIFI_FAIL_BIT{BIT1};

static constexpr const char* WIFI_AP_SSID_PREFIX  = "ESP32CrealityChamberHtr";
static constexpr const char* WIFI_AP_PASS_DEFAULT = "123456789";
static constexpr int WIFI_AP_MAX_CONN             = 4;
static constexpr int WIFI_STA_RETRY_MAX           = 5;

bool WifiMgmt::get_sta_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;

    bool ok = (nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(handle, "pass", pass, &pass_len) == ESP_OK) &&
              (strlen(ssid) > 0);
    nvs_close(handle);
    return ok;
}

void WifiMgmt::start_ap() {
    is_ap_mode = true;
    snprintf(ip_str, sizeof(ip_str), "192.168.4.1");

    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {};
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.capable = false; // Set to true to enforce PMF only connections

    /* Generate SSID with last 4 hex digits of MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid),
             "%s%02X%02X", WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
    wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
    strncpy((char *)wifi_config.ap.password, WIFI_AP_PASS_DEFAULT, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP mode started: SSID=%s, IP=%s", wifi_config.ap.ssid, ip_str);
}

void WifiMgmt::start_sta(const char *ssid, const char *password) {
    is_ap_mode = false;

    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA mode started, connecting to: %s", ssid);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "STA connection failed, starting AP mode");
        esp_wifi_stop();
        start_ap();
    }
}

WifiMgmt::~WifiMgmt() {
    // Unregister event handlers
    if (wifi_ev_instance) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev_instance));
        wifi_ev_instance = {};
    }
    if (ip_ev_instance) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_ev_instance));
        ip_ev_instance = {};
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    esp_event_loop_delete_default();

    esp_netif_deinit();

    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
}

void WifiMgmt::start() {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        [](void* arg, esp_event_base_t base, int32_t id, void* data) {
            static_cast<WifiMgmt*>(arg)->ev_handler(base, id, data);
        }, nullptr, &wifi_ev_instance
    ));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        [](void* arg, esp_event_base_t base, int32_t id, void* data) {
            static_cast<WifiMgmt*>(arg)->ev_handler(base, id, data);
        }, this, &ip_ev_instance
    ));

    /* Check for stored STA credentials */
    char ssid[33] = {0};
    char pass[65] = {0};
    if (get_sta_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No STA credentials found, starting AP mode");
        start_ap();
    }
}

bool WifiMgmt::ap_mode(void) {
    return is_ap_mode;
}

const char *WifiMgmt::get_ip(void) {
    return ip_str;
}

void WifiMgmt::set_sta_credentials(const char *ssid, const char *password) {
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved STA credentials for: %s", ssid);

    /* Restart Wi-Fi in STA mode */
    esp_wifi_stop();
    retry_count = 0;
    start_sta(ssid, password);
}

void WifiMgmt::force_ap(void) {
    esp_wifi_stop();
    start_ap();
}

void WifiMgmt::ev_handler(esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < WIFI_STA_RETRY_MAX) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retrying STA connection (%d/%d)", retry_count, WIFI_STA_RETRY_MAX);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "STA connection failed, falling back to AP mode");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected, AID=%d", event->aid);
    }
}