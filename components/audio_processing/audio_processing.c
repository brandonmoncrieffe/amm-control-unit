#include "audio_processing.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "dsps_fft2r.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_TASK_STACK_SIZE 6144U
#define AUDIO_TASK_PRIORITY 5U
#define AUDIO_DMA_DESCRIPTOR_COUNT 4U
#define AUDIO_DMA_FRAMES_PER_DESCRIPTOR 256U
#define AUDIO_READ_TIMEOUT_MS 1000U
#define AUDIO_LOG_RATE_LIMIT_US 1000000LL
#define AUDIO_DBFS_FLOOR_POWER 1.0e-12f
#define AUDIO_24_BIT_SCALE 8388608.0f
#define TWO_PI_F 6.28318530717958647692f

static const char *TAG = "audio_processing";

static audio_processing_config_t s_config;
static audio_measurement_callback_t s_callback;
static void *s_callback_context;
static i2s_chan_handle_t s_rx_channel;
static SemaphoreHandle_t s_result_mutex;
/** Guards s_config.target_frequency_hz only, which is mutable at runtime
 * via audio_processing_set_target_frequency(); everything else in s_config
 * is fixed after audio_processing_init() and needs no lock. */
static SemaphoreHandle_t s_config_mutex;
static bool s_initialized;

static int32_t s_raw_samples[AUDIO_PROCESSING_FFT_SIZE];
static float s_normalized_samples[AUDIO_PROCESSING_FFT_SIZE];
static float s_hann_window[AUDIO_PROCESSING_FFT_SIZE];
static float s_fft_data[AUDIO_PROCESSING_FFT_SIZE * 2U];
static float s_bin_power[AUDIO_PROCESSING_SPECTRUM_BIN_COUNT];
static float s_working_spectrum_dbfs[AUDIO_PROCESSING_SPECTRUM_BIN_COUNT];
static float s_spectrum_dbfs[AUDIO_PROCESSING_SPECTRUM_BIN_COUNT];
static audio_measurement_t s_latest_measurement;
static float s_window_energy;

static float power_to_dbfs(float power)
{
    return 10.0f * log10f(fmaxf(power, AUDIO_DBFS_FLOOR_POWER));
}

static esp_err_t validate_config(const audio_processing_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Configuration must not be NULL");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config->bclk_gpio),
                        ESP_ERR_INVALID_ARG, TAG,
                        "BCLK GPIO must be output-capable");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config->ws_gpio),
                        ESP_ERR_INVALID_ARG, TAG,
                        "WS GPIO must be output-capable");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(config->data_gpio),
                        ESP_ERR_INVALID_ARG, TAG,
                        "Data GPIO is invalid");
    ESP_RETURN_ON_FALSE(config->bclk_gpio != config->ws_gpio &&
                            config->bclk_gpio != config->data_gpio &&
                            config->ws_gpio != config->data_gpio,
                        ESP_ERR_INVALID_ARG, TAG,
                        "I2S GPIO assignments must be unique");
    ESP_RETURN_ON_FALSE(isfinite(config->target_half_bandwidth_hz) &&
                            config->target_half_bandwidth_hz > 0.0f,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Target half-bandwidth must be positive");

    const float nyquist_hz = AUDIO_PROCESSING_SAMPLE_RATE_HZ / 2.0f;
    for (size_t index = 0; index < AUDIO_PROCESSING_TARGET_COUNT; ++index) {
        const float target_hz = config->target_frequency_hz[index];
        ESP_RETURN_ON_FALSE(isfinite(target_hz) &&
                                target_hz - config->target_half_bandwidth_hz >=
                                    0.0f &&
                                target_hz + config->target_half_bandwidth_hz <=
                                    nyquist_hz,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Target %u band is outside 0..Nyquist",
                            (unsigned)(index + 1U));
    }

    return ESP_OK;
}

static void initialize_hann_window(void)
{
    s_window_energy = 0.0f;
    for (size_t index = 0; index < AUDIO_PROCESSING_FFT_SIZE; ++index) {
        const float phase =
            TWO_PI_F * (float)index / (float)(AUDIO_PROCESSING_FFT_SIZE - 1U);
        const float coefficient = 0.5f * (1.0f - cosf(phase));
        s_hann_window[index] = coefficient;
        s_window_energy += coefficient * coefficient;
    }
}

static esp_err_t process_audio_block(audio_measurement_t *measurement)
{
    double sample_sum = 0.0;
    for (size_t index = 0; index < AUDIO_PROCESSING_FFT_SIZE; ++index) {
        const int32_t sample_24_bit = s_raw_samples[index] >> 8;
        const float normalized = (float)sample_24_bit / AUDIO_24_BIT_SCALE;
        s_normalized_samples[index] = normalized;
        sample_sum += normalized;
    }

    const float mean = (float)(sample_sum / AUDIO_PROCESSING_FFT_SIZE);
    double square_sum = 0.0;
    for (size_t index = 0; index < AUDIO_PROCESSING_FFT_SIZE; ++index) {
        const float centered = s_normalized_samples[index] - mean;
        square_sum += (double)centered * centered;
        s_fft_data[index * 2U] = centered * s_hann_window[index];
        s_fft_data[index * 2U + 1U] = 0.0f;
    }

    ESP_RETURN_ON_ERROR(
        dsps_fft2r_fc32(s_fft_data, AUDIO_PROCESSING_FFT_SIZE), TAG,
        "FFT calculation failed");
    ESP_RETURN_ON_ERROR(
        dsps_bit_rev_fc32(s_fft_data, AUDIO_PROCESSING_FFT_SIZE), TAG,
        "FFT bit reversal failed");

    const float normalization =
        (float)AUDIO_PROCESSING_FFT_SIZE * s_window_energy;
    for (size_t bin = 0; bin < AUDIO_PROCESSING_SPECTRUM_BIN_COUNT; ++bin) {
        const float real = s_fft_data[bin * 2U];
        const float imaginary = s_fft_data[bin * 2U + 1U];
        const float single_sided_factor =
            (bin == 0U || bin == AUDIO_PROCESSING_FFT_SIZE / 2U) ? 1.0f
                                                                  : 2.0f;
        const float power = single_sided_factor *
                            (real * real + imaginary * imaginary) /
                            normalization;
        s_bin_power[bin] = power;
        s_working_spectrum_dbfs[bin] = power_to_dbfs(power);
    }

    measurement->timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    measurement->overall_rms_dbfs =
        power_to_dbfs((float)(square_sum / AUDIO_PROCESSING_FFT_SIZE));
    measurement->valid = true;

    float target_frequency_hz[AUDIO_PROCESSING_TARGET_COUNT];
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(target_frequency_hz, s_config.target_frequency_hz,
           sizeof(target_frequency_hz));
    xSemaphoreGive(s_config_mutex);

    const float bin_width_hz = (float)AUDIO_PROCESSING_SAMPLE_RATE_HZ /
                               (float)AUDIO_PROCESSING_FFT_SIZE;
    for (size_t target = 0; target < AUDIO_PROCESSING_TARGET_COUNT; ++target) {
        const float minimum_hz = target_frequency_hz[target] -
                                 s_config.target_half_bandwidth_hz;
        const float maximum_hz = target_frequency_hz[target] +
                                 s_config.target_half_bandwidth_hz;
        float band_power = 0.0f;
        for (size_t bin = 0; bin < AUDIO_PROCESSING_SPECTRUM_BIN_COUNT;
             ++bin) {
            const float frequency_hz = (float)bin * bin_width_hz;
            if (frequency_hz >= minimum_hz && frequency_hz <= maximum_hz) {
                band_power += s_bin_power[bin];
            }
        }
        measurement->target_level_dbfs[target] = power_to_dbfs(band_power);
    }

    return ESP_OK;
}

static void audio_processing_task(void *argument)
{
    (void)argument;
    size_t samples_collected = 0U;
    int64_t last_error_log_us = -AUDIO_LOG_RATE_LIMIT_US;

    while (true) {
        size_t bytes_read = 0U;
        const size_t bytes_requested =
            (AUDIO_PROCESSING_FFT_SIZE - samples_collected) *
            sizeof(s_raw_samples[0]);
        const esp_err_t error = i2s_channel_read(
            s_rx_channel, &s_raw_samples[samples_collected], bytes_requested,
            &bytes_read, pdMS_TO_TICKS(AUDIO_READ_TIMEOUT_MS));

        if (error != ESP_OK || bytes_read == 0U ||
            bytes_read % sizeof(s_raw_samples[0]) != 0U) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_error_log_us >= AUDIO_LOG_RATE_LIMIT_US) {
                ESP_LOGW(TAG, "I2S read failed: %s, bytes=%u",
                         esp_err_to_name(error), (unsigned)bytes_read);
                last_error_log_us = now_us;
            }
            samples_collected = 0U;
            continue;
        }

        samples_collected += bytes_read / sizeof(s_raw_samples[0]);
        if (samples_collected < AUDIO_PROCESSING_FFT_SIZE) {
            continue;
        }
        samples_collected = 0U;

        audio_measurement_t measurement = {0};
        if (process_audio_block(&measurement) != ESP_OK) {
            continue;
        }

        xSemaphoreTake(s_result_mutex, portMAX_DELAY);
        memcpy(s_spectrum_dbfs, s_working_spectrum_dbfs,
               sizeof(s_spectrum_dbfs));
        s_latest_measurement = measurement;
        xSemaphoreGive(s_result_mutex);

        if (s_callback != NULL) {
            s_callback(&measurement, s_callback_context);
        }
    }
}

esp_err_t audio_processing_init(const audio_processing_config_t *config,
                                audio_measurement_callback_t callback,
                                void *callback_context)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Audio processing is already initialized");
    ESP_RETURN_ON_ERROR(validate_config(config), TAG,
                        "Invalid audio configuration");

    s_result_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_result_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create result mutex");
    s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == NULL) {
        vSemaphoreDelete(s_result_mutex);
        s_result_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_config = *config;
    s_callback = callback;
    s_callback_context = callback_context;
    initialize_hann_window();

    esp_err_t error = dsps_fft2r_init_fc32(NULL, AUDIO_PROCESSING_FFT_SIZE);
    if (error != ESP_OK) {
        vSemaphoreDelete(s_result_mutex);
        s_result_mutex = NULL;
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
        return error;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = AUDIO_DMA_FRAMES_PER_DESCRIPTOR;
    error = i2s_new_channel(&channel_config, NULL, &s_rx_channel);
    if (error != ESP_OK) {
        dsps_fft2r_deinit_fc32();
        vSemaphoreDelete(s_result_mutex);
        s_result_mutex = NULL;
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
        return error;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
            AUDIO_PROCESSING_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = config->data_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    error = i2s_channel_init_std_mode(s_rx_channel, &standard_config);
    if (error == ESP_OK) {
        error = i2s_channel_enable(s_rx_channel);
    }
    if (error != ESP_OK) {
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        dsps_fft2r_deinit_fc32();
        vSemaphoreDelete(s_result_mutex);
        s_result_mutex = NULL;
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
        return error;
    }

    const BaseType_t task_created =
        xTaskCreate(audio_processing_task, "audio_processing",
                    AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        i2s_channel_disable(s_rx_channel);
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        dsps_fft2r_deinit_fc32();
        vSemaphoreDelete(s_result_mutex);
        s_result_mutex = NULL;
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "ICS-43434 input initialized: %u Hz, 32-bit left slot, %u-point FFT",
             AUDIO_PROCESSING_SAMPLE_RATE_HZ, AUDIO_PROCESSING_FFT_SIZE);
    return ESP_OK;
}

esp_err_t audio_processing_get_latest(audio_measurement_t *measurement)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Audio processing is not initialized");
    ESP_RETURN_ON_FALSE(measurement != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Measurement output must not be NULL");

    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    *measurement = s_latest_measurement;
    xSemaphoreGive(s_result_mutex);
    return measurement->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_processing_copy_latest_spectrum(float *spectrum_dbfs,
                                                size_t capacity,
                                                size_t *bin_count,
                                                uint64_t *timestamp_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Audio processing is not initialized");
    ESP_RETURN_ON_FALSE(spectrum_dbfs != NULL && bin_count != NULL &&
                            timestamp_ms != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Spectrum outputs must not be NULL");
    ESP_RETURN_ON_FALSE(capacity >= AUDIO_PROCESSING_SPECTRUM_BIN_COUNT,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "Spectrum output buffer is too small");

    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    if (!s_latest_measurement.valid) {
        xSemaphoreGive(s_result_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(spectrum_dbfs, s_spectrum_dbfs, sizeof(s_spectrum_dbfs));
    *bin_count = AUDIO_PROCESSING_SPECTRUM_BIN_COUNT;
    *timestamp_ms = s_latest_measurement.timestamp_ms;
    xSemaphoreGive(s_result_mutex);
    return ESP_OK;
}

esp_err_t audio_processing_set_target_frequency(size_t target_index,
                                                float frequency_hz)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Audio processing is not initialized");
    ESP_RETURN_ON_FALSE(target_index < AUDIO_PROCESSING_TARGET_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "Target index %u is out of range",
                        (unsigned)target_index);

    const float nyquist_hz = AUDIO_PROCESSING_SAMPLE_RATE_HZ / 2.0f;
    ESP_RETURN_ON_FALSE(
        isfinite(frequency_hz) &&
            frequency_hz - s_config.target_half_bandwidth_hz >= 0.0f &&
            frequency_hz + s_config.target_half_bandwidth_hz <= nyquist_hz,
        ESP_ERR_INVALID_ARG, TAG,
        "Requested target %.3f Hz +/- %.3f Hz band is outside 0..Nyquist",
        (double)frequency_hz, (double)s_config.target_half_bandwidth_hz);

    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    s_config.target_frequency_hz[target_index] = frequency_hz;
    xSemaphoreGive(s_config_mutex);

    ESP_LOGI(TAG, "Target %u retargeted to %.3f Hz", (unsigned)target_index,
             (double)frequency_hz);
    return ESP_OK;
}

esp_err_t audio_processing_get_target_frequency(size_t target_index,
                                                float *frequency_hz)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Audio processing is not initialized");
    ESP_RETURN_ON_FALSE(target_index < AUDIO_PROCESSING_TARGET_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "Target index %u is out of range",
                        (unsigned)target_index);
    ESP_RETURN_ON_FALSE(frequency_hz != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Frequency output must not be NULL");

    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    *frequency_hz = s_config.target_frequency_hz[target_index];
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}
