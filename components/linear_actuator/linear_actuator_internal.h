#pragma once

#include <stddef.h>

#include "servo_control.h"

typedef struct {
    float length_mm;
    float angle_deg;
} linear_actuator_point_t;

typedef struct {
    const linear_actuator_point_t *points;
    size_t point_count;
} linear_actuator_calibration_t;
