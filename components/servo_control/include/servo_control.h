#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVO_COUNT 4

#define SERVO_DEFAULT_MIN_PULSE_US 500U
#define SERVO_DEFAULT_MAX_PULSE_US 2500U
#define SERVO_DEFAULT_CENTER_OFFSET_US 0
#define SERVO_DEFAULT_MIN_ANGLE_DEG 0
#define SERVO_DEFAULT_MAX_ANGLE_DEG 180
#define SERVO_DEFAULT_INITIAL_ANGLE_DEG 0

/**
 * Default SG90 calibration. These pulse widths are starting points only and
 * must be calibrated for the specific servo and mechanical installation.
 */
#define SERVO_CALIBRATION_DEFAULT(gpio)                  \
    {                                                     \
        .gpio_num = (gpio),                               \
        .min_pulse_us = SERVO_DEFAULT_MIN_PULSE_US,       \
        .max_pulse_us = SERVO_DEFAULT_MAX_PULSE_US,       \
        .center_offset_us = SERVO_DEFAULT_CENTER_OFFSET_US, \
        .min_angle_deg = SERVO_DEFAULT_MIN_ANGLE_DEG,     \
        .max_angle_deg = SERVO_DEFAULT_MAX_ANGLE_DEG,     \
        .initial_angle_deg = SERVO_DEFAULT_INITIAL_ANGLE_DEG, \
    }

typedef enum {
    SERVO_ID_1 = 0,
    SERVO_ID_2,
    SERVO_ID_3,
    SERVO_ID_4,
    SERVO_ID_COUNT,
} servo_id_t;

typedef struct {
    int gpio_num;
    uint32_t min_pulse_us;
    uint32_t max_pulse_us;
    int32_t center_offset_us;
    int min_angle_deg;
    int max_angle_deg;
    int initial_angle_deg;
} servo_calibration_t;

typedef struct {
    servo_calibration_t servos[SERVO_COUNT];
} servo_control_config_t;

/** Initialize the shared 50 Hz LEDC timer and all four servo channels. */
esp_err_t servo_init(const servo_control_config_t *config);

/**
 * Command one servo. The requested angle is first limited to 0..180 degrees,
 * then clamped to that servo's configured permitted angle range.
 */
esp_err_t servo_set_angle(servo_id_t servo_id, int angle_deg);

/** Return the last successfully commanded (clamped) angle. */
esp_err_t servo_get_commanded_angle(servo_id_t servo_id, int *angle_deg);

/** Command every servo to the requested angle using its own calibration. */
esp_err_t servo_set_all_angles(int angle_deg);

/** Command every servo to 90 degrees, subject to its permitted angle range. */
esp_err_t servo_center_all(void);

#ifdef __cplusplus
}
#endif
