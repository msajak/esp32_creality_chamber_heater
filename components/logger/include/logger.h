#pragma once

#include <cstring>
#include <cstdint>
#include <array>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief Ring buffer logger for ESP32
 *
 * Thread-safe circular buffer that captures log messages
 * and makes them accessible via API endpoints.
 */
class RingBufferLogger {
public:
    static constexpr size_t BUF_DEPTH = 100;
    static constexpr size_t MAX_ENTRY_SIZE = 256; // Max single log entry

    static void init();

    static RingBufferLogger& instance();

    void log(const char* format, ...);

    char* get_logs_json(size_t max_entries = 0);

    void clear();

    RingBufferLogger();

    void write_entry(const char* message, size_t length, bool truncated);

private:
    struct LogEntry {
        uint8_t length{};  // Length of message data
        char data[MAX_ENTRY_SIZE]{};
    };

    std::array<LogEntry, BUF_DEPTH> buffer;
    size_t buf_head = 0;
    size_t buf_tail = 0;
    bool buf_empty = true;
    SemaphoreHandle_t lock{nullptr};

    size_t buf_ptr_next (size_t ptr){
        return (++ptr == BUF_DEPTH) ? 0 : ptr;
    }
    size_t get_entry_count() const;
};

/**
 * Initialize ESP-IDF logging to use the ring buffer
 * Must be called from app_main after initializing NVS
 */
void logger_init();
