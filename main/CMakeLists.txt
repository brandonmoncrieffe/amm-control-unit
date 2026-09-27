idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES audio_processing console linear_actuator log servo_control wifi_status_server
    INCLUDE_DIRS ""
)
