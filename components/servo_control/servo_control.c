#include "servo_control.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#define SERVO_PWM_FREQUENCY_HZ 50U
#define SERVO_PWM_PERIOD_US (1000000U / SERVO_PWM_FREQUENCY_HZ)
#define SERVO_PWM_DUTY_RESOLUTION LEDC_TIMER_14_BIT
#define SERVO_PWM_DUTY_COUNTS (1U << 14)
#define SERVO_CENTER_ANGLE_DEG 90

static const char *TAG = "servo_control";

static const ledc_channel_t s_channels[SERVO_COUNT] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
};

static servo_control_config_t s_config;
static int s_commanded_angles[SERVO_COUNT];
static bool s_initialized;

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint32_t calibration_center_pulse_us(const servo_calibration_t *calibration)
{
    const int64_t midpoint =
        ((int64_t)calibration->min_pulse_us + calibration->max_pulse_us) / 2;
    return (uint32_t)(midpoint + calibration->center_offset_us);
}

static uint32_t angle_to_pulse_us(const servo_calibration_t *calibration, int angle_deg)
{
    const uint32_t center_pulse_us = calibration_center_pulse_us(calibration);

    if (angle_deg <= SERVO_CENTER_ANGLE_DEG) {
        const uint32_t span = center_pulse_us - calibration->min_pulse_us;
        return calibration->min_pulse_us +
               (uint32_t)(((uint64_t)span * (uint32_t)angle_deg + 45U) / 90U);
    }

    const uint32_t span = calibration->max_pulse_us - center_pulse_us;
    return center_pulse_us +
           (uint32_t)(((uint64_t)span * (uint32_t)(angle_deg - 90) + 45U) / 90U);
}

static uint32_t pulse_us_to_duty(uint32_t pulse_us)
{
    return (uint32_t)(((uint64_t)pulse_us * SERVO_PWM_DUTY_COUNTS +
                       (SERVO_PWM_PERIOD_US / 2U)) /
                      SERVO_PWM_PERIOD_US);
}

static esp_err_t validate_config(const servo_control_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Configuration must not be NULL");

    for (size_t index = 0; index < SERVO_COUNT; ++index) {
        const servo_calibration_t *calibration = &config->servos[index];

        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(calibration->gpio_num),
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u GPIO %d is not output-capable",
                            (unsigned)(index + 1U), calibration->gpio_num);
        ESP_RETURN_ON_FALSE(calibration->min_pulse_us < calibration->max_pulse_us,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u pulse limits are invalid",
                            (unsigned)(index + 1U));
        ESP_RETURN_ON_FALSE(calibration->max_pulse_us < SERVO_PWM_PERIOD_US,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u maximum pulse must be less than the PWM period",
                            (unsigned)(index + 1U));
        ESP_RETURN_ON_FALSE(calibration->min_angle_deg >= 0 &&
                                calibration->min_angle_deg <= calibration->max_angle_deg &&
                                calibration->max_angle_deg <= 180,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u angle limits must be within 0..180",
                            (unsigned)(index + 1U));
        ESP_RETURN_ON_FALSE(calibration->initial_angle_deg >=
                                    calibration->min_angle_deg &&
                                calibration->initial_angle_deg <=
                                    calibration->max_angle_deg,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u initial angle is outside its permitted range",
                            (unsigned)(index + 1U));

        const int64_t center_pulse_us =
            ((int64_t)calibration->min_pulse_us + calibration->max_pulse_us) / 2 +
            calibration->center_offset_us;
        ESP_RETURN_ON_FALSE(center_pulse_us > calibration->min_pulse_us &&
                                center_pulse_us < calibration->max_pulse_us,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Servo %u center offset places center outside pulse limits",
                            (unsigned)(index + 1U));

        for (size_t other = index + 1U; other < SERVO_COUNT; ++other) {
            ESP_RETURN_ON_FALSE(calibration->gpio_num !=
                                    config->servos[other].gpio_num,
                                ESP_ERR_INVALID_ARG, TAG,
                                "Servos %u and %u use the same GPIO",
                                (unsigned)(index + 1U), (unsigned)(other + 1U));
        }
    }

    return ESP_OK;
}

esp_err_t servo_init(const servo_control_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Servo control is already initialized");
    ESP_RETURN_ON_ERROR(validate_config(config), TAG,
                        "Invalid servo configuration");

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_DUTY_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG,
                        "Failed to configure LEDC timer");

    s_config = *config;

    for (size_t index = 0; index < SERVO_COUNT; ++index) {
        const servo_calibration_t *calibration = &s_config.servos[index];
        const int initial_angle = clamp_int(calibration->initial_angle_deg,
                                            calibration->min_angle_deg,
                                            calibration->max_angle_deg);
        const uint32_t pulse_us = angle_to_pulse_us(calibration, initial_angle);
        const ledc_channel_config_t channel_config = {
            .gpio_num = calibration->gpio_num,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = s_channels[index],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = pulse_us_to_duty(pulse_us),
            .hpoint = 0,
            .flags.output_invert = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG,
                            "Failed to configure servo %u channel",
                            (unsigned)(index + 1U));

        s_commanded_angles[index] = initial_angle;
        ESP_LOGI(TAG,
                 "Servo %u initialized: requested=%d deg, clamped=%d deg, "
                 "pulse=%" PRIu32 " us, GPIO=%d",
                 (unsigned)(index + 1U), calibration->initial_angle_deg,
                 initial_angle, pulse_us, calibration->gpio_num);
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t servo_set_angle(servo_id_t servo_id, int angle_deg)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Servo control is not initialized");
    ESP_RETURN_ON_FALSE(servo_id >= SERVO_ID_1 && servo_id < SERVO_ID_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid servo ID %d",
                        (int)servo_id);

    const servo_calibration_t *calibration = &s_config.servos[servo_id];
    int clamped_angle = clamp_int(angle_deg, 0, 180);
    clamped_angle = clamp_int(clamped_angle, calibration->min_angle_deg,
                              calibration->max_angle_deg);

    const uint32_t pulse_us = angle_to_pulse_us(calibration, clamped_angle);
    const uint32_t duty = pulse_us_to_duty(pulse_us);
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channels[servo_id], duty), TAG,
        "Failed to set servo %d duty", (int)servo_id + 1);
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channels[servo_id]), TAG,
        "Failed to apply servo %d duty", (int)servo_id + 1);

    s_commanded_angles[servo_id] = clamped_angle;
    ESP_LOGI(TAG,
             "Servo %d: requested=%d deg, clamped=%d deg, pulse=%" PRIu32
             " us",
             (int)servo_id + 1, angle_deg, clamped_angle, pulse_us);
    return ESP_OK;
}

esp_err_t servo_get_commanded_angle(servo_id_t servo_id, int *angle_deg)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Servo control is not initialized");
    ESP_RETURN_ON_FALSE(servo_id >= SERVO_ID_1 && servo_id < SERVO_ID_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid servo ID %d",
                        (int)servo_id);
    ESP_RETURN_ON_FALSE(angle_deg != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Angle output must not be NULL");

    *angle_deg = s_commanded_angles[servo_id];
    return ESP_OK;
}

esp_err_t servo_set_all_angles(int angle_deg)
{
    esp_err_t first_error = ESP_OK;

    for (servo_id_t servo_id = SERVO_ID_1; servo_id < SERVO_ID_COUNT;
         servo_id = (servo_id_t)(servo_id + 1)) {
        const esp_err_t error = servo_set_angle(servo_id, angle_deg);
        if (first_error == ESP_OK && error != ESP_OK) {
            first_error = error;
        }
    }

    return first_error;
}

esp_err_t servo_center_all(void)
{
    return servo_set_all_angles(SERVO_CENTER_ANGLE_DEG);
}
