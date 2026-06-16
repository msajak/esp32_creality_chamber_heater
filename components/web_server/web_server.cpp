#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_mgmt.h"
#include "ota.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "logger.h"

#include "web_server.h"

static constexpr const char* TAG = "WEB";

/* Embedded HTML file */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

esp_err_t WebServer::root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

esp_err_t WebServer::api_params_get_handler(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    bool fail{};
    if (buf_len > 1) {
        char buf[buf_len]{};
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGD("HTTPD_params", "Found query string: %s", buf);

            auto parseParam = [&buf](const char* param_name, auto&& extractParam, bool optional = false){
                char param_value[32]{};
                if (httpd_query_key_value(buf, param_name, param_value, sizeof(param_value)) == ESP_OK) {
                    ESP_LOGD("HTTPD_params", "Found parameter '%s' with value: %s", param_name, param_value);
                    return extractParam(param_value);
                } else {
                    ESP_LOGD("HTTPD_params", "Not Found parameter '%s', optional=%d", param_name, (int)optional);
                    return optional;
                }
            };

            int temp{}, time{};
            bool clear_fault = false;
            bool watchdog_refresh = false;

            if (!parseParam("temp", [&](const char* str_val){
                temp = atoi(str_val);
                return temp >= 0;
            })) {
                ESP_LOGW("HTTPD_params", "Missing or invalid temp");
                fail = true;
            }

            if (!parseParam("time", [&](const char* str_val){
                time = atoi(str_val);
                return time >= 0;
            })) {
                ESP_LOGW("HTTPD_params", "Missing or invalid time");
                fail = true;
            }

            if (!parseParam("clear_fault", [&](const char* str_val){
                bool ok{};
                if (strcmp(str_val, "true") == 0) {
                    clear_fault = true;
                    ok = true;
                } else if (strcmp(str_val, "false") == 0) {
                    clear_fault = false;
                    ok = true;
                }
                return ok;
            }, true)) {
                ESP_LOGW("HTTPD_params", "Invalid clear_fault");
                fail = true;
            }

            if (!parseParam("watchdog_refresh", [&](const char* str_val){
                bool ok{};
                if (strcmp(str_val, "true") == 0) {
                    watchdog_refresh = true;
                    ok = true;
                } else if (strcmp(str_val, "false") == 0) {
                    watchdog_refresh = false;
                    ok = true;
                }
                return ok;
            }, true)) {
                ESP_LOGW("HTTPD_params", "Invalid watchdog_refresh");
                fail = true;
            }

            auto validate_params = [](const Params& p) {
                if (p.watchdog_refresh) return true;
                return (p.temp == 0 && p.time == 0) || (p.temp >= 20 && p.temp <= 70 && p.time > 0 && p.time <= 600);
            };

            if (!fail) {
                Params params{.temp=temp, .time=time, .clear_fault = clear_fault, .watchdog_refresh = watchdog_refresh};
                if (!validate_params(params)) {
                    ESP_LOGW("HTTPD_params", "Invalid Params temp=%d, time=%d, setting to 0", params.temp, params.time);
                    params.temp = 0;
                    params.time = 0;
                    params.clear_fault = false;
                    params.watchdog_refresh = false;
                };
                if (xQueueOverwrite(params_queue, &params) == pdPASS) {
                    ESP_LOGI("HTTPD_params", "Params temp=%d, time=%d clear_fault=%d watchdog_refresh=%d queued",
                             params.temp, params.time, params.clear_fault, params.watchdog_refresh);
                    char resp[128];
                    sprintf(resp, "Stored params: temp=%d, time=%d clear_fault=%d watchdog_refresh=%d\n",
                            params.temp, params.time, params.clear_fault, params.watchdog_refresh);
                    httpd_resp_send(req, resp, strlen(resp));
                } else {
                    ESP_LOGE("HTTPD_params", "Queue write failed");
                    fail = true;
                }
            }
        }
    } else {
        ESP_LOGI("HTTPD_params", "params not received");
    }

    if (fail) {
        httpd_resp_send_500(req);
    }

    return ESP_OK;
}

esp_err_t WebServer::api_wifi_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid/pass");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Connecting to new network...\"}");

    wifi_mgmt.set_sta_credentials(ssid->valuestring, pass->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebServer::api_ota_post_handler(httpd_req_t *req) {
    return ota_upload_handler(req);
}

esp_err_t WebServer::api_status_get_handler(httpd_req_t *req) {
    Status last_status{};
    if (xQueuePeek(status_queue, &last_status, 0) == pdTRUE) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "timenow", last_status.timenow);
        cJSON_AddNumberToObject(root, "temp_ntc", last_status.temp_ntc);
        cJSON_AddNumberToObject(root, "temp_filtered", last_status.temp_filtered);
        cJSON_AddBoolToObject(root, "temp_filtered_ok", last_status.temp_filtered_ok);
        cJSON_AddNumberToObject(root, "setpoint", last_status.setpoint);
        cJSON_AddNumberToObject(root, "temp_error", last_status.temp_error);
        cJSON_AddBoolToObject(root, "heating_active", last_status.heating_active);
        cJSON_AddNumberToObject(root, "duty", (float)last_status.duty * 100 / last_status.duty_max);
        cJSON_AddNumberToObject(root, "heater_timeout_sec", last_status.heater_timeout_sec);
        cJSON_AddNumberToObject(root, "heater_time_left_sec", last_status.heater_time_left_sec);
        cJSON_AddBoolToObject(root, "fan_status", last_status.fan_status);
        cJSON_AddNumberToObject(root, "last_rise_fan1", last_status.last_rise_fan1);
        cJSON_AddNumberToObject(root, "edge_count_fan1", last_status.edge_count_fan1);
        cJSON_AddNumberToObject(root, "last_rise_fan2", last_status.last_rise_fan2);
        cJSON_AddNumberToObject(root, "edge_count_fan2", last_status.edge_count_fan2);
        cJSON_AddStringToObject(root, "fault_reason", to_string(last_status.fault_reason).c_str());
        cJSON_AddStringToObject(root, "version", version_);

        char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        cJSON_Delete(root);
        free(json_str);
        return ESP_OK;
    } else {
        return ESP_FAIL;
    }
}

esp_err_t WebServer::api_logs_get_handler(httpd_req_t *req) {
    // Get optional query parameter for max entries
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    size_t max_entries = 0;

    if (buf_len > 1) {
        char buf[buf_len]{};
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param_value[32]{};
            if (httpd_query_key_value(buf, "max", param_value, sizeof(param_value)) == ESP_OK) {
                max_entries = atoi(param_value);
            }
        }
    }

    char *json_logs = RingBufferLogger::instance().get_logs_json(max_entries);

    if (json_logs) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_logs);
        free(json_logs);
        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate logs JSON");
        return ESP_FAIL;
    }
}

esp_err_t WebServer::api_logs_clear_handler(httpd_req_t *req) {
    RingBufferLogger::instance().clear();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "msg", "Logs cleared");

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    cJSON_Delete(root);
    free(json_str);

    return ESP_OK;
}

void WebServer::handle_panda_settings(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "WS: failed to parse JSON");
        return;
    }

    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (!cJSON_IsObject(settings)) {
        ESP_LOGW(TAG, "WS: missing or invalid 'settings' key");
        cJSON_Delete(root);
        return;
    }

    cJSON *item;
    bool should_apply = false;

    if ((item = cJSON_GetObjectItem(settings, "work_mode")) && cJSON_IsNumber(item))
        panda_state_.work_mode = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "set_temp")) && cJSON_IsNumber(item))
        panda_state_.set_temp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "temp")) && cJSON_IsNumber(item))
        panda_state_.temp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "filtertemp")) && cJSON_IsNumber(item))
        panda_state_.filtertemp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "hotbedtemp")) && cJSON_IsNumber(item))
        panda_state_.hotbedtemp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "isrunning")) && cJSON_IsNumber(item))
        panda_state_.isrunning = (item->valueint != 0);
    if ((item = cJSON_GetObjectItem(settings, "filament_temp")) && cJSON_IsNumber(item))
        panda_state_.filament_temp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "filament_timer")) && cJSON_IsNumber(item))
        panda_state_.filament_timer = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "custom_temp")) && cJSON_IsNumber(item))
        panda_state_.custom_temp = item->valueint;
    if ((item = cJSON_GetObjectItem(settings, "custom_timer")) && cJSON_IsNumber(item))
        panda_state_.custom_timer = item->valueint;

    if ((item = cJSON_GetObjectItem(settings, "work_on"))) {
        if (cJSON_IsBool(item))
            panda_state_.work_on = cJSON_IsTrue(item);
        else if (cJSON_IsNumber(item))
            panda_state_.work_on = (item->valueint != 0);
        should_apply = true;
    }

    cJSON_Delete(root);

    if (should_apply)
        apply_panda_state();
}

void WebServer::apply_panda_state(bool watchdog_refresh) {
    Params params{};

    if (panda_state_.work_on) {
        int target = 0;
        switch (panda_state_.work_mode) {
            case 1: target = panda_state_.temp; break;
            case 2: target = panda_state_.set_temp; break;
            case 3: target = panda_state_.filament_temp > 0
                           ? panda_state_.filament_temp
                           : panda_state_.custom_temp; break;
        }
        if (target < 0) target = 0;
        if (target > 80) target = 80;
        params.temp = target;
        params.time = 0;
        params.clear_fault = !watchdog_refresh;
        params.watchdog_refresh = watchdog_refresh;
    }

    if (xQueueOverwrite(params_queue, &params) == pdPASS) {
        if (watchdog_refresh) {
            ESP_LOGD(TAG, "Panda: watchdog refresh");
        } else {
            ESP_LOGI(TAG, "Panda: work_on=%d mode=%d temp=%d",
                     panda_state_.work_on, panda_state_.work_mode, params.temp);
        }
    }
}

esp_err_t WebServer::send_panda_state(httpd_req_t *req) {
    Status status{};
    bool have_status = xQueuePeek(status_queue, &status, 0) == pdTRUE;

    cJSON *root = cJSON_CreateObject();
    cJSON *settings = cJSON_CreateObject();

    if (have_status) {
        cJSON_AddNumberToObject(settings, "cal_warehouse_temp", status.temp_filtered);
        cJSON_AddNumberToObject(settings, "warehouse_temper", status.temp_ntc);
    }
    cJSON_AddNumberToObject(settings, "work_mode", panda_state_.work_mode);
    cJSON_AddBoolToObject(settings, "work_on", panda_state_.work_on);
    cJSON_AddNumberToObject(settings, "set_temp", panda_state_.set_temp);
    cJSON_AddNumberToObject(settings, "temp", panda_state_.temp);
    cJSON_AddNumberToObject(settings, "filtertemp", panda_state_.filtertemp);
    cJSON_AddNumberToObject(settings, "hotbedtemp", panda_state_.hotbedtemp);
    cJSON_AddNumberToObject(settings, "isrunning", panda_state_.isrunning ? 1 : 0);
    cJSON_AddNumberToObject(settings, "remaining_seconds", 0);
    cJSON_AddStringToObject(settings, "fw_version", "V1.0.3");
    cJSON_AddNumberToObject(settings, "printer_type", 2);

    cJSON_AddItemToObject(root, "settings", settings);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGD(TAG, "WS TX: %s", json);

    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(json);
    frame.len = strlen(json);

    esp_err_t ret = httpd_ws_send_frame(req, &frame);
    free(json);
    return ret;
}

void WebServer::send_panda_state_async() {
    if (!server) return;

    size_t max_clients = max_open_sockets_;
    int fds[max_clients];
    if (httpd_get_client_list(server, &max_clients, fds) != ESP_OK || max_clients == 0)
        return;

    char *json = nullptr;

    for (size_t i = 0; i < max_clients; i++) {
        httpd_ws_client_info_t info = httpd_ws_get_fd_info(server, fds[i]);
        ESP_LOGD(TAG, "WS broadcast: fd=%d info=%d", fds[i], info);
        if (info != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;

        if (!json) {
            Status status{};
            bool have_status = xQueuePeek(status_queue, &status, 0) == pdTRUE;

            cJSON *root = cJSON_CreateObject();
            cJSON *settings = cJSON_CreateObject();
            if (have_status) {
                cJSON_AddNumberToObject(settings, "cal_warehouse_temp", status.temp_filtered);
                cJSON_AddNumberToObject(settings, "warehouse_temper", status.temp_ntc);
            }
            cJSON_AddNumberToObject(settings, "work_mode", panda_state_.work_mode);
            cJSON_AddBoolToObject(settings, "work_on", panda_state_.work_on);
            cJSON_AddNumberToObject(settings, "set_temp", panda_state_.set_temp);
            cJSON_AddNumberToObject(settings, "temp", panda_state_.temp);
            cJSON_AddNumberToObject(settings, "filtertemp", panda_state_.filtertemp);
            cJSON_AddNumberToObject(settings, "hotbedtemp", panda_state_.hotbedtemp);
            cJSON_AddNumberToObject(settings, "isrunning", panda_state_.isrunning ? 1 : 0);
            cJSON_AddNumberToObject(settings, "remaining_seconds", 0);
            cJSON_AddStringToObject(settings, "fw_version", "V1.0.3");
            cJSON_AddNumberToObject(settings, "printer_type", 2);
            cJSON_AddItemToObject(root, "settings", settings);

            json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (!json) return;
        }

        if (panda_state_.work_on)
            apply_panda_state(true);

        httpd_ws_frame_t frame{};
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t*>(json);
        frame.len = strlen(json);
        httpd_ws_send_frame_async(server, fds[i], &frame);
    }

    free(json);
}

static esp_err_t send_ws_text(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
    frame.len = strlen(text);
    ESP_LOGI(TAG, "WS TX: %s", text);
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t WebServer::websocket_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS handshake received");
        return ESP_OK;
    }

    char rx_buf[512];
    httpd_ws_frame_t ws_pkt{};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = reinterpret_cast<uint8_t*>(rx_buf);

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(rx_buf) - 1);
    if (ret != ESP_OK || ws_pkt.len == 0)
        return ret;

    rx_buf[ws_pkt.len] = '\0';
    ESP_LOGI(TAG, "WS RX: %s", rx_buf);

    cJSON *root = cJSON_Parse(rx_buf);
    if (!root) {
        ESP_LOGW(TAG, "WS: failed to parse JSON");
        return ESP_OK;
    }

    if (cJSON_GetObjectItem(root, "settings")) {
        char *settings_json = cJSON_PrintUnformatted(root);
        if (settings_json) {
            handle_panda_settings(settings_json);
            free(settings_json);
        }
        if (cJSON_GetObjectItem(cJSON_GetObjectItem(root, "settings"), "printer_type")) {
            send_ws_text(req, "{\"response\":{\"type\":\"printer_type\",\"ok\":1}}");
        }
    }

    cJSON *printer = cJSON_GetObjectItem(root, "printer");
    if (printer) {
        if (cJSON_GetObjectItem(printer, "disconnect")) {
            ESP_LOGI(TAG, "WS: printer disconnect (stub)");
            send_ws_text(req, "{\"printer\":{\"state\":0}}");
        } else if (cJSON_GetObjectItem(printer, "ip") || cJSON_GetObjectItem(printer, "name")) {
            ESP_LOGI(TAG, "WS: printer bind (stub)");
            send_ws_text(req, "{\"printer\":{\"state\":3}}");
        }
    }

    cJSON_Delete(root);

    send_panda_state(req);
    return ESP_OK;
}

WebServer::~WebServer() {
    if (server) {
        httpd_stop(server);
        server = {};
    }
}

void WebServer::start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    max_open_sockets_ = config.max_open_sockets;

    if (server) {
        ESP_LOGE(TAG, "HTTP server already started");
        return;
    }

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    auto make_uri = [this](const char *path, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *), bool ws = false) {
        httpd_uri_t u = {};
        u.uri = path;
        u.method = method;
        u.handler = fn;
        u.user_ctx = this;
        u.is_websocket = ws;
        return u;
    };

    httpd_uri_t uris[] = {
        make_uri("/", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->root_get_handler(req);
        }),
        make_uri("/api/params", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_params_get_handler(req);
        }),
        make_uri("/api/status", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_status_get_handler(req);
        }),
        make_uri("/api/wifi/config", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_wifi_post_handler(req);
        }),
        make_uri("/api/ota/update", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_ota_post_handler(req);
        }),
        make_uri("/api/logs", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_logs_get_handler(req);
        }),
        make_uri("/api/logs/clear", HTTP_POST, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->api_logs_clear_handler(req);
        }),
        make_uri("/ws", HTTP_GET, [](httpd_req_t *req) {
            return static_cast<WebServer*>(req->user_ctx)->websocket_handler(req);
        }, true),
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WebServer*>(arg)->send_panda_state_async();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_broadcast",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t ws_timer;
    if (esp_timer_create(&timer_args, &ws_timer) == ESP_OK) {
        esp_timer_start_periodic(ws_timer, 2000000);
    }

    ESP_LOGI(TAG, "HTTP server started");
}
