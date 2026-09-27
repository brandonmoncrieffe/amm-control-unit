#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PROCESSING_SAMPLE_RATE_HZ 16000U
#define AUDIO_PROCESSING_FFT_SIZE 2048U
#define AUDIO_PROCESSING_SPECTRUM_BIN_COUNT \
    (AUDIO_PROCESSING_FFT_SIZE / 2U + 1U)
#define AUDIO_PROCESSING_TARGET_COUNT 4U

typedef struct {
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t data_gpio;
    float target_frequency_hz[AUDIO_PROCESSING_TARGET_COUNT];
    float target_half_bandwidth_hz;
} audio_processing_config_t;

typedef struct {
    uint64_t timestamp_ms;
    float target_level_dbfs[AUDIO_PROCESSING_TARGET_COUNT];
    float overall_rms_dbfs;
    bool valid;
} audio_measurement_t;

/**
 * Called from the audio acquisition task after a block has been processed.
 * The callback must not block or perform lengthy work.
 */
typedef void (*audio_measurement_callback_t)(
    const audio_measurement_t *measurement, void *context);

/** Initialize I2S acquisition, DSP resources, and the processing task. */
esp_err_t audio_processing_init(const audio_processing_config_t *config,
                                audio_measurement_callback_t callback,
                                void *callback_context);

/** Copy the most recent target-band and RMS measurement. */
esp_err_t audio_processing_get_latest(audio_measurement_t *measurement);

/**
 * Copy the latest single-sided full spectrum in relative dBFS. The destination
 * must hold AUDIO_PROCESSING_SPECTRUM_BIN_COUNT floats.
 */
esp_err_t audio_processing_copy_latest_spectrum(float *spectrum_dbfs,
                                                size_t capacity,
                                                size_t *bin_count,
                                                uint64_t *timestamp_ms);

#ifdef __cplusplus
}
#endif
