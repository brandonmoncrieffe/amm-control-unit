#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "servo_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float requested_length_mm;
    float clamped_length_mm;
    int commanded_angle_deg;
} linear_actuator_command_result_t;

/** Return true when a build-time CSV calibration exists for this servo. */
bool linear_actuator_is_calibrated(servo_id_t servo_id);

/** Return the inclusive calibrated length range in millimetres. */
esp_err_t linear_actuator_get_range(servo_id_t servo_id, float *minimum_mm,
                                    float *maximum_mm);

/**
 * Convert a requested length to an interpolated angle and command the servo.
 * Lengths outside the calibrated range are clamped to the nearest endpoint.
 */
esp_err_t linear_actuator_set_length(
    servo_id_t servo_id, float requested_length_mm,
    linear_actuator_command_result_t *result);

/** Estimate length from the servo's last commanded angle. */
esp_err_t linear_actuator_get_estimated_length(servo_id_t servo_id,
                                               float *length_mm);

#ifdef __cplusplus
}
#endif
