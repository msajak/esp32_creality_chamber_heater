
#include "freertos/FreeRTOS.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_cali.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

#include "util.h"

#include "pid_ctrl.h"

// GPIO pins
static constexpr gpio_num_t LED_GPIO             = GPIO_NUM_22;
static constexpr gpio_num_t MOSFET_GPIO          = GPIO_NUM_25;
static constexpr gpio_num_t FAN_GPIO             = GPIO_NUM_26;
static constexpr gpio_num_t FAN_RPM_GPIO_1       = GPIO_NUM_27;
static constexpr gpio_num_t FAN_RPM_GPIO_2       = GPIO_NUM_14;
static constexpr adc_channel_t THERMISTOR_ADC_CH = ADC_CHANNEL_6;

// PWM settings
static constexpr int PWM_FREQ_HZ        = 10;
static constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_10_BIT;
static constexpr int PWM_MAX            = (1 << 10) - 1;

// PID and control timings (ms)
static constexpr int64_t US_PER_SEC       = 1000000;
static constexpr int64_t PID_INTERVAL_US  = 1 * US_PER_SEC;
static constexpr int64_t FAN_COOLDOWN_US  = 60 * US_PER_SEC;
static constexpr int64_t STALL_TIMEOUT_US = 180 * US_PER_SEC; // 3min
static constexpr int64_t FAN_SPINUP_US    = 5 * US_PER_SEC;

// Temperature range and thresholds
static constexpr float TEMP_SMOOTH_ALPHA  = 0.5; // Smoothing factor (0.0 to 1.0)
static constexpr uint16_t FILTER_MIN_LOOPS  = 10;
static constexpr float TEMP_RANGE_MIN     = 1.0f;
static constexpr float TEMP_RANGE_MAX     = 150.0f;
static constexpr float STALL_MIN_RISE     = 1.0f;
static constexpr uint32_t STALL_MIN_DUTY  = (uint32_t)(PWM_MAX * 0.5f);
static constexpr float STALL_ERROR_THRESH = 5.0f;

static constexpr const char* TAG = "PID";

void IRAM_ATTR PidCtrl::handle_fan_sensor_pulse(int64_t now, gpio_num_t pin) {
    int level = gpio_get_level(pin);
    if (pin == FAN_RPM_GPIO_1) {
        if (level) { // posedge
            last_rise_fan1_.store(now);
            edge_count_fan1_.fetch_add(1);
        }
        sensor_level_fan1_.store(level);
    } else {
        if (level) { // posedge
            last_rise_fan2_.store(now);
            edge_count_fan2_.fetch_add(1);
        }
        sensor_level_fan2_.store(level);
    }
}

PidCtrl::PidCtrl(QueueHandle_t& params_queue, QueueHandle_t& status_queue): params_queue(params_queue), status_queue(status_queue){
    init_adc();
    init_pwm();
    init_fan();
    init_pid(64.0f, 2.5f, 0.0f);
    reset_temp_filter();

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    duty_cycle_ = 0;
}

PidCtrl::~PidCtrl() {
    if (timer_) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Delete the PID task
    if (pid_task_handle_) {
        vTaskDelete(pid_task_handle_);
        pid_task_handle_ = nullptr;
    }

    // Remove GPIO ISR handlers
    gpio_isr_handler_remove(FAN_RPM_GPIO_1);
    gpio_isr_handler_remove(FAN_RPM_GPIO_2);

    // Clean up ADC and calibration handles
    if (cali_handle_ != nullptr) {
        adc_cali_delete_scheme_line_fitting(cali_handle_);
        cali_handle_ = nullptr;
    }

    if (adc1_handle_ != nullptr) {
        adc_oneshot_del_unit(adc1_handle_);
        adc1_handle_ = nullptr;
    }
}

bool PidCtrl::start() {
    // Create a PID task that will run the heavy PID loop when notified by timer ISR
    BaseType_t rc = xTaskCreatePinnedToCore(
        [](void* arg) {
            // PID task loop: wait for notification from timer ISR and run the PID loop with captured timestamp
            PidCtrl *self = static_cast<PidCtrl*>(arg);
            for (;;) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                int64_t ts = self->last_timer_ts_.exchange(0);
                if (ts == 0) {
                    ts = esp_timer_get_time();
                    ESP_LOGW(TAG, "suprious PID task notify without a stored timestamp");
                }
                self->pid_timenow_ = ts;
                self->pid_loop();
            }
        },
        "pid_task",
        4096,
        this,
        tskIDLE_PRIORITY + 2,
        &pid_task_handle_,
        tskNO_AFFINITY
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PID task");
        return false;
    }

    // Create an ISR-dispatched esp_timer that only captures timestamp and notifies the task
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            PidCtrl *self = static_cast<PidCtrl*>(arg);
            int64_t now = esp_timer_get_time();
            self->last_timer_ts_.store(now);
            xTaskNotifyGive(self->pid_task_handle_);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pid_timer",
        .skip_unhandled_events = true,
    };

    if (esp_timer_create(&timer_args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer");
        return false;
    }
    if (esp_timer_start_periodic(timer_, PID_INTERVAL_US) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer");
        return false;
    }
    ESP_LOGI(TAG, "PID loop (deferred) started");
    return true;
}

void PidCtrl::init_adc() {
    adc_oneshot_unit_init_cfg_t init_config1{};
    init_config1.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle_));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle_, THERMISTOR_ADC_CH, &config));

    adc_cali_line_fitting_config_t cali_config{};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = ADC_ATTEN_DB_11;
    cali_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle_));
}

void PidCtrl::init_pwm() {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_RESOLUTION,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = MOSFET_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {}
    };
    ledc_channel_config(&channel);
}

void PidCtrl::init_fan() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FAN_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(FAN_GPIO, 0);

    gpio_config_t rpm_conf = {
        .pin_bit_mask = (1ULL << FAN_RPM_GPIO_1) | (1ULL << FAN_RPM_GPIO_2),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&rpm_conf);

    gpio_install_isr_service(0);
    // register non-capturing lambdas as ISR handlers; pass 'this' as arg
    gpio_isr_handler_add(FAN_RPM_GPIO_1, [](void *arg) {
        PidCtrl *self = static_cast<PidCtrl*>(arg);
        self->handle_fan_sensor_pulse(esp_timer_get_time(), FAN_RPM_GPIO_1);
    }, this);
    gpio_isr_handler_add(FAN_RPM_GPIO_2, [](void *arg) {
        PidCtrl *self = static_cast<PidCtrl*>(arg);
        self->handle_fan_sensor_pulse(esp_timer_get_time(), FAN_RPM_GPIO_2);
    }, this);
}

void PidCtrl::set_fault(FaultReason fault) {
    fault_reason_ = fault_reason_ | fault;
}

void PidCtrl::clear_fault(FaultReason fault) {
    if (fault == FaultReason::NONE) {
        fault_reason_ = FaultReason::NONE;
    } else {
        fault_reason_ &= ~fault;
    }
}

bool PidCtrl::has_fault(FaultReason fault) {
    if (fault == FaultReason::NONE) {
        return fault_reason_ != FaultReason::NONE;
    } else {
        return check_fault_reason(fault_reason_, fault);
    }
}

void PidCtrl::init_pid(float kp, float ki, float kd) {
    pid_state_.kp = kp;
    pid_state_.ki = ki;
    pid_state_.kd = kd;
    pid_state_.reset_setpoint();
    pid_state_.prev_error = 0.0f;
    pid_state_.output_min = 0.0f;
    pid_state_.output_max = static_cast<float>(PWM_MAX);
}

bool PidCtrl::check_temp_range(float temp_c) {
    return (temp_c >= TEMP_RANGE_MIN && temp_c <= TEMP_RANGE_MAX && !isnan(temp_c));
}

void PidCtrl::reset_temp_filter() {
    temp_ntc_filtered = 0.0f;
    temp_filter_warmup_loops = 0;
    temp_filtered_ok = false;
}

float PidCtrl::compute_pid(float measured, float dt) {
    float error = pid_state_.setpoint - measured;
    float p_term = pid_state_.kp * error;
    pid_state_.integral += error * dt;
    float i_term = pid_state_.ki * pid_state_.integral;
    float derivative = (error - pid_state_.prev_error) / dt;
    float d_term = pid_state_.kd * derivative;
    pid_state_.prev_error = error;

    float output = p_term + i_term + d_term;
    if (output > pid_state_.output_max) {
        output = pid_state_.output_max;
        if (error > 0) pid_state_.integral -= error * dt;
    } else if (output < pid_state_.output_min) {
        output = pid_state_.output_min;
        if (error < 0) pid_state_.integral -= error * dt;
    }
    return output;
}

void PidCtrl::manage_fans() {
    auto set_fan = [&](bool on, const char *reason) {
        auto was_on = fan_status_;
        if (fan_status_ != on) {
            ESP_LOGI(TAG, "change to: %s reason: %s", on ? "ON" : "OFF", reason);
        }
        fan_status_ = on;

        if (on && !was_on) {
            fan_on_time_ = pid_timenow_;
            last_rise_fan1_.store(pid_timenow_);
            last_rise_fan2_.store(pid_timenow_);
            fan_speed_ok_ = false;
        } else if (!on) {
            fan_speed_ok_ = false;
        }
        gpio_set_level(FAN_GPIO, on ? 1 : 0);
    };

    if (heating_active_) {
        set_fan(true, "heating_active");
        heating_was_active_ = true;
    } else if (heating_was_active_) {
        heater_off_time_ = pid_timenow_;
        heating_was_active_ = false;
        set_fan(true, "heating_was_active");
    } else {
        if (heater_off_time_ != 0) {
            int64_t elapsed_us = pid_timenow_ - heater_off_time_;
            if (elapsed_us < FAN_COOLDOWN_US) {
                set_fan(true, "in_cooldown");
            } else {
                set_fan(false, "out_of_cooldown");
            }
        } else {
            set_fan(false, "heater_never_on");
        }
    }

    fan_state_.fans_nominal = false;
    if (fan_status_) {
        int64_t fan_on_time = pid_timenow_ - fan_on_time_;
        if (fan_on_time >= FAN_SPINUP_US) {

            // no rising edges since the last pid calc = spinnig at full speed
            const bool fan_sensor_ok = !sensor_level_fan1_ && !sensor_level_fan2_;
            const bool no_speed_drop = (pid_timenow_ - fan_state_.fan1_last_edge) > PID_INTERVAL_US && (pid_timenow_ - fan_state_.fan2_last_edge) > PID_INTERVAL_US;
            fan_state_.fans_nominal = fan_sensor_ok && no_speed_drop;
            if (fan_state_.fans_nominal) {
                fan_speed_ok_ = true;
            } else if (fan_speed_ok_) {
                if (!has_fault(FaultReason::FAN_DROP)) {
                    ESP_LOGE(TAG, "FAULT: fan speed dropped below nominal");
                    set_fault(FaultReason::FAN_DROP);
                }
            } else {
                if (!has_fault(FaultReason::FAN_TIMEOUT)) {
                    ESP_LOGE(TAG, "FAULT: fans did not reach nominal speed within %ds", (int)(FAN_SPINUP_US / US_PER_SEC));
                    set_fault(FaultReason::FAN_TIMEOUT);
                }
            }
        }
    }
}

void PidCtrl::apply_duty_cycle() {
    if (duty_cycle_ > PWM_MAX) duty_cycle_ = PWM_MAX;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle_);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void PidCtrl::publish_status() {
    Status current_status{};
    current_status.timenow = pid_timenow_;
    current_status.temp_ntc = temp_ntc_c_;
    current_status.temp_filtered = temp_ntc_filtered;
    current_status.temp_filtered_ok = temp_filtered_ok;
    current_status.setpoint = pid_state_.setpoint;
    current_status.temp_error = pid_state_.setpoint - temp_ntc_c_;
    current_status.heating_active = heating_active_;
    current_status.duty = duty_cycle_;
    current_status.duty_max = PWM_MAX;
    current_status.heater_timeout_sec = heater_timeout_sec_;
    current_status.heater_time_left_sec = heating_time_left_sec_;
    current_status.fan_status = fan_status_;
    current_status.last_rise_fan1 = fan_state_.fan1_last_edge;
    current_status.edge_count_fan1 = fan_state_.fan1_edges;
    current_status.last_rise_fan2 = fan_state_.fan2_last_edge;
    current_status.edge_count_fan2 = fan_state_.fan2_edges;
    current_status.fault_reason = fault_reason_;

    if (xQueueOverwrite(status_queue, &current_status) != pdPASS) {
        ESP_LOGE(TAG, "Status Queue write failed");
    }
}

Params PidCtrl::get_params() {
    Params params{};
    if (xQueueReceive(params_queue, &params, 0) == pdTRUE) {
        bool keep_integral = pid_state_.setpoint > 30.0f && std::abs(pid_state_.setpoint - params.temp) <= 3.0;
        if (!keep_integral) {
            pid_state_.integral = 0.0f;
        }
        ESP_LOGI(TAG, "Command: setpoint %.1f -> %d, timeout %lus, integral=%.1f, integral_kept=%d",
            pid_state_.setpoint, params.temp, (unsigned long)params.time, pid_state_.integral, (int)keep_integral);
        pid_state_.setpoint = params.temp;
        heater_timeout_sec_ = params.time;
        if (params.clear_fault) {
            clear_fault();
            stall_check_time_ = 0;
        }
        fan_speed_ok_ = false;
        if (params.temp > 0) {
            heater_start_time_ = pid_timenow_;
        }
    }
    return params;
}

float PidCtrl::read_temperature() {
    float temp_celsius = 0.0f;

    int raw_val;
    int val_mv;
    bool ok = true;
    if (adc_oneshot_read(adc1_handle_, THERMISTOR_ADC_CH, &raw_val) != ESP_OK) {
        ok = false;
    }
    if (ok && adc_cali_raw_to_voltage(cali_handle_, raw_val, &val_mv) != ESP_OK) {
        ok = false;
    }
    if (ok) {
        auto ntc_mv_to_celsius = [](int mv) {
            constexpr float NTC_RO = 100000.0f;
            constexpr float NTC_T0 = 298.15f;
            constexpr float NTC_BETA = 3950.0f;
            constexpr float SERIES_R = 47000.0f;
            constexpr float V_SUPPLY_MV = 3300.0f;
            float r_ntc = SERIES_R * (float)mv / (V_SUPPLY_MV - (float)mv);
            float t_kelvin = 1.0f / (1.0f / NTC_T0 + logf(r_ntc / NTC_RO) / NTC_BETA);
            return t_kelvin - 273.15f;
        };
        temp_celsius = ntc_mv_to_celsius(val_mv);
        ESP_LOGD(TAG, "reading: val_mv=%d, raw_val=%d calc'd temp_celsius=%f", val_mv, raw_val, temp_celsius);
    } else {
        set_fault(FaultReason::TEMP_READ);
        ESP_LOGW(TAG, "failed to read ADC");
    }
    return temp_celsius;
}

void PidCtrl::pid_loop() {
    // Capture and clear fan sensor edge counters in task context
    fan_state_.fan1_last_edge = last_rise_fan1_.load();
    fan_state_.fan2_last_edge = last_rise_fan2_.load();
    fan_state_.fan1_edges = edge_count_fan1_.exchange(0);
    fan_state_.fan2_edges = edge_count_fan2_.exchange(0);
    fan_state_.fan1_sensor = sensor_level_fan1_.load();
    fan_state_.fan2_sensor = sensor_level_fan2_.load();

    temp_ntc_c_ = {};
    heating_active_ = {};
    heating_time_left_sec_ = {};
    duty_cycle_ = 0;

    ScopeGuard set_duty_guard([&]() { // protect against any unexpected scope exits
        if (has_fault()) {
            pid_state_.reset_setpoint();
            duty_cycle_ = 0;
        }
        apply_duty_cycle();
    });

    {
        get_params();

        temp_ntc_c_ = read_temperature();

        heating_active_ = !has_fault() && pid_state_.setpoint > 0.0f;

        if (heating_active_) {
            int64_t elapsed_us = pid_timenow_ - heater_start_time_;
            if (heater_timeout_sec_ > 0) {
                int64_t heater_timeout_us = heater_timeout_sec_ * US_PER_SEC;
                if (elapsed_us >= heater_timeout_us) {
                    ESP_LOGW(TAG, "Safety timeout (%lus) reached — switching off heater, elapsed_us=%lld timenow %lld heater_start_time %lld",
                                (unsigned long)heater_timeout_sec_, elapsed_us, pid_timenow_, heater_start_time_);
                    pid_state_.reset_setpoint();
                    set_fault(FaultReason::HEAT_TIMEOUT);
                    heater_timeout_sec_ = 0;
                }
                heating_time_left_sec_ = (heater_timeout_us - elapsed_us) / US_PER_SEC;
            } else {
                ESP_LOGW(TAG, "No safety timeout — switching off heater, elapsed_us=%lld timenow %lld heater_start_time %lld", elapsed_us, pid_timenow_, heater_start_time_);
                pid_state_.reset_setpoint();
                set_fault(FaultReason::HEAT_TIMEOUT);
            }
        }

        if (!check_temp_range(temp_ntc_c_)) {
            if (!has_fault(FaultReason::TEMP_RANGE)) {
                ESP_LOGE(TAG, "FAULT: temperature reading %.1f°C error or out of range [%.0f–%.0f]", temp_ntc_c_, TEMP_RANGE_MIN, TEMP_RANGE_MAX);
                set_fault(FaultReason::TEMP_RANGE);
                pid_state_.reset_setpoint();
                reset_temp_filter();
            }
        } else {
            if (temp_ntc_filtered == 0.0f) {
                temp_ntc_filtered = temp_ntc_c_;
                temp_filter_warmup_loops = 0;
                temp_filtered_ok = false;
            } else {
                temp_ntc_filtered = (temp_ntc_c_ * TEMP_SMOOTH_ALPHA) + (temp_ntc_filtered * (1.0f - TEMP_SMOOTH_ALPHA));
                if (!temp_filtered_ok) {
                    if (temp_filter_warmup_loops < FILTER_MIN_LOOPS) {
                        ++temp_filter_warmup_loops;
                        if (!has_fault(FaultReason::TEMP_FILT)) {
                            set_fault(FaultReason::TEMP_FILT);
                        }
                    } else {
                        temp_filtered_ok = true;
                        clear_fault(FaultReason::TEMP_FILT);
                        ESP_LOGI(TAG, "temp_ntc_filtered stabilized at %.1f°C", temp_ntc_filtered);
                    }
                }
            }
        }

        if (!check_temp_range(temp_ntc_filtered)) {
            if (temp_filtered_ok && !has_fault(FaultReason::TEMP_FILT_RANGE)) {
                ESP_LOGE(TAG, "FAULT: filtered temperature %.1f°C error or out of range [%.0f–%.0f]", temp_ntc_filtered, TEMP_RANGE_MIN, TEMP_RANGE_MAX);
                set_fault(FaultReason::TEMP_RANGE);
                pid_state_.reset_setpoint();
                reset_temp_filter();
            }
        }

        if (has_fault()) {
            heating_active_ = false;
            pid_state_.reset_setpoint();
            heater_timeout_sec_ = 0;
            duty_cycle_ = {};
            pid_state_.integral = 0.0f;
            pid_state_.prev_error = 0.0f;
            stall_check_time_ = {};
            stall_baseline_temp_ = {};
        } else {
            float dt = PID_INTERVAL_US;
            duty_cycle_ = static_cast<uint32_t>(compute_pid(temp_ntc_filtered, dt));

            if (!has_fault(FaultReason::TEMP_STUCK)) {
                float error = pid_state_.setpoint - temp_ntc_filtered;
                if (duty_cycle_ > STALL_MIN_DUTY && error > STALL_ERROR_THRESH) {
                    if (stall_check_time_ == 0) {
                        stall_check_time_ = pid_timenow_;
                        stall_baseline_temp_ = temp_ntc_filtered;
                    } else {
                        int64_t elapsed_us = pid_timenow_ - stall_check_time_;
                        if (elapsed_us >= (int64_t)STALL_TIMEOUT_US) {
                            if (temp_ntc_filtered < stall_baseline_temp_ + STALL_MIN_RISE) {
                                ESP_LOGE(TAG, "FAULT: no temperature rise in %ds (%.1f°C -> %.1f°C)", (int)(STALL_TIMEOUT_US / US_PER_SEC), stall_baseline_temp_, temp_ntc_filtered);
                                set_fault(FaultReason::TEMP_STUCK);
                            } else {
                                stall_check_time_ = pid_timenow_;
                                stall_baseline_temp_ = temp_ntc_filtered;
                            }
                        }
                    }
                } else {
                    stall_check_time_ = 0;
                }
            }

        }
        manage_fans();
    }

    publish_status();

    ESP_LOGI(TAG, "fault=%s T=%.1f°C Tf=%.1f°C fw=%d set=%.1f°C err=%.1f I=%.1f heat=%s duty=%lu/%d heat_timeout=%lu, heat_time_left=%lu, fans=%s fans_speed=%s, sensors [lvl/last_rise/edges] fan1: [%d/%lld/%lld] fan2: [%d/%lld/%lld]",
                to_string(fault_reason_).c_str(),
                temp_ntc_c_, temp_ntc_filtered, temp_filter_warmup_loops, pid_state_.setpoint, pid_state_.setpoint - temp_ntc_c_, pid_state_.integral,
                heating_active_ ? "ON" : "OFF", (unsigned long)duty_cycle_, PWM_MAX, heater_timeout_sec_, heating_time_left_sec_,
                fan_status_ ? "ON" : "OFF", fan_state_.fans_nominal ? "OK" : "BAD", fan_state_.fan1_sensor, fan_state_.fan1_last_edge, fan_state_.fan1_edges, fan_state_.fan2_sensor, fan_state_.fan2_last_edge, fan_state_.fan2_edges
            );

    gpio_set_level(LED_GPIO, led_state);
    led_state = !led_state;
}
