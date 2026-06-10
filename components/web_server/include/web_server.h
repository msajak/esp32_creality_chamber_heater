#pragma once

#include "esp_http_server.h"
#include "esp_err.h"
#include "shared_objs.h"

class Params;
class WifiMgmt;

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

public:
    WebServer(QueueHandle_t& params_queue, QueueHandle_t& status_queue, WifiMgmt& wifi_mgmt):
        params_queue(params_queue), status_queue(status_queue), wifi_mgmt(wifi_mgmt) {}
    ~WebServer();

    void start();

    QueueHandle_t& params_queue;
    QueueHandle_t& status_queue;
    WifiMgmt& wifi_mgmt;
    Status last_status;
};
