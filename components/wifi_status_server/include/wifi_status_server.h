#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STATUS_SERVER_SERVO_COUNT 4U
#define WIFI_STATUS_SERVER_TARGET_COUNT 4U

typedef struct {
    int servo_gpio[WIFI_STATUS_SERVER_SERVO_COUNT];
    float target_frequency_hz[WIFI_STATUS_SERVER_TARGET_COUNT];
} wifi_status_server_config_t;

/**
 * Bring up a WiFi access point and an HTTP server exposing read-only JSON
 * status endpoints:
 *
 *   GET /state    - servo commanded angles plus the latest target-band and
 *                   RMS audio measurement.
 *   GET /spectrum - the latest complete FFT in relative dBFS.
 *
 * This never sends commands to servo_control or audio_processing; it only
 * reads their existing public getters. It is independent of the USB
 * Serial/JTAG console and safe to run alongside it.
 */
esp_err_t wifi_status_server_init(const wifi_status_server_config_t *config);

#ifdef __cplusplus
}
#endif
