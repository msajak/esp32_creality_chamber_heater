#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "shared_objs.h"
#include "web_server.h"
#include "wifi_mgmt.h"
#include "pid_ctrl.h"
#include "logger.h"

// ESP-IDF entry point
extern "C" {
    void app_main(void);
}

QueueHandle_t params_queue;
QueueHandle_t status_queue;

static constexpr const char* TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 Chamber Heater Controller starting...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");

    logger_init();
    ESP_LOGI(TAG, "Logger initialized");

    params_queue = xQueueCreate(1, sizeof(Params));
    if (!params_queue) {
        ESP_LOGE(TAG, "Failed to create params queue");
        return;
    }

    status_queue = xQueueCreate(1, sizeof(Status));
    if (!status_queue) {
        ESP_LOGE(TAG, "Failed to create status queue");
        return;
    }

    WifiMgmt wifi_mgmt;
    wifi_mgmt.start();

    WebServer server(params_queue, status_queue, wifi_mgmt);
    server.start();

    ESP_LOGI(TAG, "ESP32 Chamber Heater Controller started. Web UI at http://%s/", wifi_mgmt.get_ip());

    PidCtrl pid_ctrl(params_queue, status_queue);
    if (!pid_ctrl.start()) {
        ESP_LOGE(TAG, "PID loop failed to start");
    }

    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
