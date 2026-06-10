#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string>

struct Params {
    int temp{};
    int time{};
    bool clear_fault{};
};

enum class FaultReason: uint16_t {
    NONE            = 0,
    HEAT_TIMEOUT    = 1 << 0,
    TEMP_READ       = 1 << 1,
    TEMP_RANGE      = 1 << 2,
    TEMP_FILT       = 1 << 3,
    TEMP_FILT_RANGE = 1 << 4,
    TEMP_STUCK      = 1 << 5,
    FAN_TIMEOUT     = 1 << 6,
    FAN_DROP        = 1 << 7
};

struct Status {
    int64_t timenow;
    float temp_ntc;
    float temp_filtered;
    bool temp_filtered_ok;
    float setpoint;
    float temp_error;
    bool heating_active;
    uint32_t duty;
    uint32_t duty_max;
    uint32_t heater_timeout_sec;
    uint32_t heater_time_left_sec;
    bool fan_status;
    int64_t last_rise_fan1;
    int64_t edge_count_fan1;
    int64_t last_rise_fan2;
    int64_t edge_count_fan2;
    FaultReason fault_reason;
};

constexpr FaultReason operator|(FaultReason lhs, FaultReason rhs) {
    using T = std::underlying_type_t<FaultReason>;
    return static_cast<FaultReason>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

constexpr FaultReason operator&(FaultReason lhs, FaultReason rhs) {
    using T = std::underlying_type_t<FaultReason>;
    return static_cast<FaultReason>(static_cast<T>(lhs) & static_cast<T>(rhs));
}

constexpr FaultReason operator~(FaultReason rhs) {
    using T = std::underlying_type_t<FaultReason>;
    return static_cast<FaultReason>(~static_cast<T>(rhs));
}

constexpr FaultReason& operator&=(FaultReason& lhs, FaultReason rhs) {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool check_fault_reason(FaultReason fault, FaultReason flag) {
    return (fault & flag) == flag;
}

std::string to_string(FaultReason fault_reason);