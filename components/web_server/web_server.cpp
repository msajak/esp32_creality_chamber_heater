#include <string.h>
#include <stdio.h>
#include "esp_log.h"
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

            auto validate_params = [](const Params& p) {
                return (p.temp == 0 && p.time == 0) || (p.temp >= 20 && p.temp <= 70 && p.time > 0 && p.time <= 600);
            };

            if (!fail) {
                Params params{.temp=temp, .time=time, .clear_fault = clear_fault};
                if (!validate_params(params)) {
                    ESP_LOGW("HTTPD_params", "Invalid Params temp=%d, time=%d, setting to 0", params.temp, params.time);
                    params.temp = 0;
                    params.time = 0;
                    params.clear_fault = false;
                };
                if (xQueueOverwrite(params_queue, &params) == pdPASS) {
                    ESP_LOGI("HTTPD_params", "Params temp=%d, time=%d clear_fault=%d queued", params.temp, params.time, params.clear_fault);
                    char resp[100];
                    sprintf(resp, "Stored params: temp=%d, time=%d clear_fault=%d\n", params.temp, params.time, params.clear_fault);
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

esp_err_t WebServer::websocket_handler(httpd_req_t *req) {
    // If this is the initial HTTP GET handshake request, just return ESP_OK to upgrade
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake received, upgrading connection to WebSocket");
        return ESP_OK;
    }

    // Loop indefinitely to keep the connection alive and process incoming data
    while (true) {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(ws_pkt));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        // 1. Read frame header (with a 0-byte payload buffer to get length/type)
        // Note: This will block until a frame arrives or the socket times out
        esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
        if (ret == ESP_ERR_TIMEOUT) {
            continue; // Keep looping if no data arrived within the timeout window
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket recv header failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGD(TAG, "WebSocket frame: type=%d, len=%zu, fin=%d", ws_pkt.type, ws_pkt.len, ws_pkt.final);

        // 2. Handle control frames
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            ESP_LOGI(TAG, "WebSocket client requested close");
            httpd_ws_frame_t close_frame;
            memset(&close_frame, 0, sizeof(close_frame));
            close_frame.type = HTTPD_WS_TYPE_CLOSE;
            httpd_ws_send_frame(req, &close_frame);
            return ESP_OK; // Exit the loop and handler cleanly
        }

        if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            ESP_LOGI(TAG, "WebSocket ping received, sending pong");
            httpd_ws_frame_t pong_frame;
            memset(&pong_frame, 0, sizeof(pong_frame));
            pong_frame.type = HTTPD_WS_TYPE_PONG;
            ret = httpd_ws_send_frame(req, &pong_frame);
            if (ret != ESP_OK) return ret;
            continue;
        }

        if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
            ESP_LOGD(TAG, "WebSocket pong received");
            continue;
        }

        // Handle zero-length data frames
        if (ws_pkt.len == 0) {
            continue;
        }

        if (ws_pkt.len > 4096) {
            ESP_LOGW(TAG, "WebSocket frame too large (%zu > 4096)", ws_pkt.len);
            return ESP_ERR_INVALID_SIZE;
        }

        // 3. Allocate and read the payload data
        char *buf = static_cast<char*>(malloc(ws_pkt.len + 1));
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for payload", ws_pkt.len);
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = reinterpret_cast<uint8_t*>(buf);
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket recv payload failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }

        buf[ws_pkt.len] = '\0';
        ESP_LOGI(TAG, "WebSocket RX (%zu bytes, type=%d): %s", ws_pkt.len, ws_pkt.type, buf);

        // 4. Echo the text back to client
        httpd_ws_frame_t out_pkt;
        memset(&out_pkt, 0, sizeof(out_pkt));
        out_pkt.payload = reinterpret_cast<uint8_t*>(buf);
        out_pkt.len = ws_pkt.len;
        out_pkt.type = HTTPD_WS_TYPE_TEXT;

        ret = httpd_ws_send_frame(req, &out_pkt);
        free(buf);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket send failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
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

    if (server) {
        ESP_LOGE(TAG, "HTTP server already started");
        return;
    }

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET,  .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->root_get_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/params", .method = HTTP_GET, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_params_get_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/status", .method = HTTP_GET, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_status_get_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/wifi/config", .method = HTTP_POST, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_wifi_post_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/ota/update",  .method = HTTP_POST, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_ota_post_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_logs_get_handler(req);
        }, .user_ctx = this},
        {.uri = "/api/logs/clear", .method = HTTP_POST, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->api_logs_clear_handler(req);
        }, .user_ctx = this},
        {.uri = "/ws",  .method = HTTP_GET, .handler = [](httpd_req_t *req) {
            WebServer* instance = static_cast<WebServer*>(req->user_ctx);
            return instance->websocket_handler(req);
        }, .user_ctx = this, .is_websocket = true}
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started");
}
