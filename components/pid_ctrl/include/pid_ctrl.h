#pragma once

#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include <atomic>

#include "shared_objs.h"

class PidCtrl {
public:
    PidCtrl(QueueHandle_t& params_queue, QueueHandle_t& status_queue);
    ~PidCtrl();

    bool start();

private:
    PidCtrl(const PidCtrl &) = delete;
    PidCtrl &operator=(const PidCtrl &) = delete;

    void init_adc();
    void init_pwm();
    void init_fan();
    void init_pid(float kp, float ki, float kd);
    void set_fault(FaultReason fault);
    void clear_fault(FaultReason fault = FaultReason::NONE);
    bool has_fault(FaultReason fault = FaultReason::NONE);
    bool check_temp_range(float temp_celsius);
    void reset_temp_filter();
    float compute_pid(float measured, float dt);
    void manage_fans();
    void apply_duty_cycle();
    void publish_status();

    Params get_params();
    float read_temperature();
    void pid_loop();

    void handle_fan_sensor_pulse(int64_t now, gpio_num_t pin);

    struct PidState {
        float kp{};
        float ki{};
        float kd{};
        float setpoint{};
        float integral{};
        float prev_error{};
        float output_min{};
        float output_max{};

        void reset_setpoint() {
            setpoint = 0.0f;
            integral = 0.0f;
        }
    } pid_state_{};

    struct FanState {
        int64_t fan1_last_edge{};
        int64_t fan2_last_edge{};
        int64_t fan1_edges{};
        int64_t fan2_edges{};
        bool fan1_sensor{};
        bool fan2_sensor{};
        bool fans_nominal{};
    } fan_state_{};


    QueueHandle_t& params_queue;
    QueueHandle_t& status_queue;

    // Timer -> task hybrid: ISR stores timestamp here and notifies the PID task
    std::atomic<int64_t> last_timer_ts_{};
    TaskHandle_t pid_task_handle_ = NULL;
    esp_timer_handle_t timer_ = NULL;

    adc_oneshot_unit_handle_t adc1_handle_{};
    adc_cali_handle_t cali_handle_ = NULL;

    int64_t pid_timenow_{};

    float temp_ntc_c_{};

    float temp_ntc_filtered{};
    uint16_t temp_filter_warmup_loops{};
    bool temp_filtered_ok{};

    int64_t heater_start_time_{};
    int64_t heater_off_time_{};
    uint32_t heating_time_left_sec_{};
    uint32_t heater_timeout_sec_{};
    bool heating_active_{};
    bool heating_was_active_{};
    FaultReason fault_reason_{};
    float stall_baseline_temp_{};
    int64_t stall_check_time_{};

    bool fan_status_{};
    int64_t fan_on_time_{};
    bool fan_speed_ok_{};

    std::atomic<int64_t> last_rise_fan1_{};
    std::atomic<int64_t> last_rise_fan2_{};
    std::atomic<int64_t> edge_count_fan1_{};
    std::atomic<int64_t> edge_count_fan2_{};
    std::atomic<bool> sensor_level_fan1_{true}; // 1: not spinning
    std::atomic<bool> sensor_level_fan2_{true}; // 1: not spinning

    uint32_t duty_cycle_ = 0;

    bool led_state{};
};
