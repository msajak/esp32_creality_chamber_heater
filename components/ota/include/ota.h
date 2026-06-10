#pragma once

#include "esp_http_server.h"

/* Handle OTA firmware upload via HTTP POST.
 * Streams the binary directly to the inactive OTA partition. */
esp_err_t ota_upload_handler(httpd_req_t *req);
