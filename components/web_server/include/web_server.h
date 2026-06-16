#pragma once

#include "esp_http_server.h"
#include "esp_err.h"
#include "shared_objs.h"

class Params;
class WifiMgmt;

struct PandaState {
    int work_mode = 2;     // 1=auto, 2=always_on, 3=filament_drying
    bool work_on = false;
    int set_temp = 0;      // target for work_mode 2
    int temp = 0;          // target for work_mode 1 (auto)
    int filtertemp = 30;
    int hotbedtemp = 80;
    bool isrunning = false;
    int filament_temp = 55;
    int filament_timer = 6;
    int custom_temp = 55;
    int custom_timer = 6;
};

class WebServer {

    httpd_handle_t server{};

    esp_err_t root_get_handler(httpd_req_t *req);
    esp_err_t api_params_get_handler(httpd_req_t *req);
    esp_err_t api_wifi_post_handler(httpd_req_t *req);
    esp_err_t api_ota_post_handler(httpd_req_t *req);
    esp_err_t api_status_get_handler(httpd_req_t *req);
    esp_err_t api_logs_get_handler(httpd_req_t *req);
    esp_err_t api_logs_clear_handler(httpd_req_t *req);
    esp_err_t websocket_handler(httpd_req_t *req);

    PandaState panda_state_;
    int ws_client_fd_ = -1;
    size_t max_open_sockets_ = 7;
    const char* version_ = "v1.0.0";
    void handle_panda_settings(const char *json_str);
    void apply_panda_state(bool watchdog_refresh = false);
    esp_err_t send_panda_state(httpd_req_t *req);
    void send_panda_state_async();

public:
    WebServer(QueueHandle_t& params_queue, QueueHandle_t& status_queue, WifiMgmt& wifi_mgmt, const char* version = "v1.0.0"):
        version_(version), params_queue(params_queue), status_queue(status_queue), wifi_mgmt(wifi_mgmt) {}
    ~WebServer();

    void start();

    QueueHandle_t& params_queue;
    QueueHandle_t& status_queue;
    WifiMgmt& wifi_mgmt;
    Status last_status;
};
