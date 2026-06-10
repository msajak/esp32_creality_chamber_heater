#include "logger.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <string>

static RingBufferLogger g_logger;

RingBufferLogger& RingBufferLogger::instance() {
    return g_logger;
}

void RingBufferLogger::init() {

}

RingBufferLogger::RingBufferLogger() {
    lock = xSemaphoreCreateMutex();
}

void RingBufferLogger::write_entry(const char* message, size_t length, bool truncated) {

    if (truncated) {
        length = MAX_ENTRY_SIZE - 4;
    }

    length = std::min(length, MAX_ENTRY_SIZE - 1);

    if (xSemaphoreTake(lock, 0) != pdTRUE) return;

    std::memcpy(&buffer[buf_head].data, message, length);
    if (truncated) {
        // If message is still too long, try to truncate and add ...
        buffer[buf_head].data[length] = '.';
        buffer[buf_head].data[length + 1] = '.';
        buffer[buf_head].data[length + 2] = '.';
        buffer[buf_head].data[length + 3] = 0;
        buffer[buf_head].length = MAX_ENTRY_SIZE - 1;
    } else {
        buffer[buf_head].data[length] = 0;
        buffer[buf_head].length = length;
    }

    if (!buf_empty && buf_head == buf_tail) {
        buf_tail = buf_ptr_next(buf_tail);
    }
    buf_head = buf_ptr_next(buf_head);

    buf_empty = false;
    xSemaphoreGive(lock);
}

char* RingBufferLogger::get_logs_json(size_t max_entries) {
    if (xSemaphoreTake(lock, 0) != pdTRUE) return nullptr;

    cJSON *root = cJSON_CreateArray();

    size_t entries_read = 0;
    size_t entry_ptr = buf_tail;
    do {
        cJSON_AddItemToArray(root, cJSON_CreateString(buffer[entry_ptr].data));
        if (++entries_read == max_entries) {
            break;
        }
        entry_ptr = buf_ptr_next(entry_ptr);
    } while (entry_ptr != buf_head);

    xSemaphoreGive(lock);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    return json_str;
}

void RingBufferLogger::clear() {
    if (xSemaphoreTake(lock, 0) != pdTRUE) return;
    buf_head = 0;
    buf_tail = 0;
    buf_empty = true;
    xSemaphoreGive(lock);
}

size_t RingBufferLogger::get_entry_count() const {
    size_t entry_count = 0;
    if (!buf_empty) {
        if (buf_head == buf_tail) {
            entry_count = BUF_DEPTH;
        } else if (buf_head > buf_tail) {
            entry_count = buf_head - buf_tail;
        } else {
            entry_count = buf_head + (BUF_DEPTH - buf_tail);
            // should never happen since we don't support pop ops
        }
    }
    return entry_count;
}

// ESP-IDF logging hook
static int vprintf_hook(const char *fmt, va_list args) {
    char buffer[RingBufferLogger::MAX_ENTRY_SIZE] = {0};
    int len = vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);

    if (len > 0) {
        bool truncated = len >= sizeof(buffer) - 1;
        RingBufferLogger::instance().write_entry(buffer, len, truncated);
    }

    // Also print to serial/default output
    return vprintf(fmt, args);
}

void logger_init() {
    RingBufferLogger::init();

    // Register the vprintf hook with ESP-IDF
    esp_log_set_vprintf(&vprintf_hook);

    ESP_LOGI("LOGGER", "Ring buffer logger initialized");
}
