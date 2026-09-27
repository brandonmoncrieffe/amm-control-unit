idf_component_register(
    SRCS "audio_processing.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_driver_i2s esp_timer esp-dsp
)
