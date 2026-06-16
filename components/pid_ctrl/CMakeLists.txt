idf_component_register(
    SRCS "pid_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_adc driver freertos shared esp_driver_gpio esp_driver_ledc
)
