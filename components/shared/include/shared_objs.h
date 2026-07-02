#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string>

struct PandaState {
    int work_mode = {};     // 1=auto, 2=always_on, 3=filament_drying
    bool work_on = false;
    int set_temp = 0;      // target for work_mode 2
    int temp = 0;          // target for work_mode 1 (auto)
    int filtertemp = {};
    int hotbedtemp = {};
    bool isrunning = false;
    int filament_temp = {};
    int filament_timer = {};
    int custom_temp = {};
    int custom_timer = {};
};

struct Params {
    int temp{};
    int time{};
    bool clear_fault{};
    bool watchdog_refresh{};
    PandaState panda_state{};
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
    FAN_DROP        = 1 << 7,
    TEMP_OVERTEMP   = 1 << 8
};

enum class ControllerState : uint8_t {
    IDLE = 0,
    HEATING = 1,
    COOLDOWN = 2,
    FAULT = 3
};

inline std::string to_string(ControllerState state) {
    switch (state) {
        case ControllerState::IDLE: return "IDLE";
        case ControllerState::HEATING: return "HEATING";
        case ControllerState::COOLDOWN: return "COOLDOWN";
        case ControllerState::FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

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
    FaultReason fault_history;
    ControllerState state = ControllerState::IDLE;
    int panda_work_mode = {};
    bool panda_work_on = false;
    bool panda_isrunning = false;
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