#include "linear_actuator.h"

#include <math.h>
#include <stddef.h>

#include "linear_actuator_calibration_data.h"

static bool servo_id_is_valid(servo_id_t servo_id)
{
    return servo_id >= SERVO_ID_1 && servo_id < SERVO_ID_COUNT;
}

static const linear_actuator_calibration_t *get_calibration(
    servo_id_t servo_id)
{
    if (!servo_id_is_valid(servo_id)) {
        return NULL;
    }
    const linear_actuator_calibration_t *calibration =
        &g_linear_actuator_calibrations[servo_id];
    return calibration->point_count >= 2U ? calibration : NULL;
}

bool linear_actuator_is_calibrated(servo_id_t servo_id)
{
    return get_calibration(servo_id) != NULL;
}

esp_err_t linear_actuator_get_range(servo_id_t servo_id, float *minimum_mm,
                                    float *maximum_mm)
{
    if (!servo_id_is_valid(servo_id) || minimum_mm == NULL ||
        maximum_mm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const linear_actuator_calibration_t *calibration =
        get_calibration(servo_id);
    if (calibration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *minimum_mm = calibration->points[0].length_mm;
    *maximum_mm =
        calibration->points[calibration->point_count - 1U].length_mm;
    return ESP_OK;
}

static float length_to_angle(const linear_actuator_calibration_t *calibration,
                             float length_mm)
{
    for (size_t index = 0U; index + 1U < calibration->point_count; ++index) {
        const linear_actuator_point_t *low = &calibration->points[index];
        const linear_actuator_point_t *high =
            &calibration->points[index + 1U];
        if (length_mm <= high->length_mm) {
            const float fraction = (length_mm - low->length_mm) /
                                   (high->length_mm - low->length_mm);
            return low->angle_deg +
                   fraction * (high->angle_deg - low->angle_deg);
        }
    }
    return calibration->points[calibration->point_count - 1U].angle_deg;
}

esp_err_t linear_actuator_set_length(
    servo_id_t servo_id, float requested_length_mm,
    linear_actuator_command_result_t *result)
{
    if (!servo_id_is_valid(servo_id) || !isfinite(requested_length_mm) ||
        result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const linear_actuator_calibration_t *calibration =
        get_calibration(servo_id);
    if (calibration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const float minimum_mm = calibration->points[0].length_mm;
    const float maximum_mm =
        calibration->points[calibration->point_count - 1U].length_mm;
    const float clamped_mm =
        fminf(fmaxf(requested_length_mm, minimum_mm), maximum_mm);
    const int angle_deg = (int)lroundf(length_to_angle(calibration, clamped_mm));

    result->requested_length_mm = requested_length_mm;
    result->clamped_length_mm = clamped_mm;
    result->commanded_angle_deg = angle_deg;
    return servo_set_angle(servo_id, angle_deg);
}

esp_err_t linear_actuator_get_estimated_length(servo_id_t servo_id,
                                               float *length_mm)
{
    if (!servo_id_is_valid(servo_id) || length_mm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const linear_actuator_calibration_t *calibration =
        get_calibration(servo_id);
    if (calibration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int angle_deg = 0;
    esp_err_t error = servo_get_commanded_angle(servo_id, &angle_deg);
    if (error != ESP_OK) {
        return error;
    }

    const float angle = (float)angle_deg;
    for (size_t index = 0U; index + 1U < calibration->point_count; ++index) {
        const linear_actuator_point_t *first = &calibration->points[index];
        const linear_actuator_point_t *second =
            &calibration->points[index + 1U];
        const float minimum_angle = fminf(first->angle_deg, second->angle_deg);
        const float maximum_angle = fmaxf(first->angle_deg, second->angle_deg);
        if (angle >= minimum_angle && angle <= maximum_angle) {
            const float fraction = (angle - first->angle_deg) /
                                   (second->angle_deg - first->angle_deg);
            *length_mm = first->length_mm +
                         fraction * (second->length_mm - first->length_mm);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}
