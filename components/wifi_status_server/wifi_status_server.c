#include "wifi_status_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio_processing.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "servo_control.h"

#define WIFI_STATUS_JSON_BUFFER_SIZE 16384U

static const char *TAG = "wifi_status_server";

static wifi_status_server_config_t s_config;
static httpd_handle_t s_httpd;
static char s_json_buffer[WIFI_STATUS_JSON_BUFFER_SIZE];
static float s_spectrum_dbfs[AUDIO_PROCESSING_SPECTRUM_BIN_COUNT];

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac),
                  event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac),
                  event->aid);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
        result = nvs_flash_init();
    }
    return result;
}

static esp_err_t start_access_point(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "Failed to create default event loop");
    (void)esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG,
                        "Failed to init WiFi driver");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL, NULL),
        TAG, "Failed to register WiFi event handler");

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_AMM_WIFI_AP_SSID,
            .ssid_len = strlen(CONFIG_AMM_WIFI_AP_SSID),
            .channel = CONFIG_AMM_WIFI_AP_CHANNEL,
            .password = CONFIG_AMM_WIFI_AP_PASSWORD,
            .max_connection = CONFIG_AMM_WIFI_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG,
                        "Failed to set WiFi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG,
                        "Failed to set WiFi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi");

    ESP_LOGI(TAG, "Access point '%s' started on channel %d, join it and "
                  "reach the status API at http://192.168.4.1",
             CONFIG_AMM_WIFI_AP_SSID, CONFIG_AMM_WIFI_AP_CHANNEL);
    return ESP_OK;
}

static esp_err_t append_state_json(char *buffer, size_t capacity)
{
    int offset = snprintf(buffer, capacity, "{\"servos\":[");

    for (size_t index = 0; index < WIFI_STATUS_SERVER_SERVO_COUNT; ++index) {
        int angle_deg = 0;
        (void)servo_get_commanded_angle((servo_id_t)index, &angle_deg);
        offset += snprintf(buffer + offset, capacity - (size_t)offset,
                           "%s{\"id\":%u,\"gpio\":%d,\"angle_deg\":%d}",
                           index == 0U ? "" : ",", (unsigned)(index + 1U),
                           s_config.servo_gpio[index], angle_deg);
        if (offset < 0 || (size_t)offset >= capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    offset += snprintf(buffer + offset, capacity - (size_t)offset,
                       "],\"audio\":");

    audio_measurement_t measurement;
    if (audio_processing_get_latest(&measurement) == ESP_OK) {
        /* Read live, not the frozen config snapshot: targets are mutable at
         * runtime via /target (see audio_processing_set_target_frequency). */
        float current_targets_hz[WIFI_STATUS_SERVER_TARGET_COUNT];
        for (size_t index = 0; index < WIFI_STATUS_SERVER_TARGET_COUNT; ++index) {
            current_targets_hz[index] = s_config.target_frequency_hz[index];
            (void)audio_processing_get_target_frequency(index,
                                                        &current_targets_hz[index]);
        }
        offset += snprintf(
            buffer + offset, capacity - (size_t)offset,
            "{\"timestamp_ms\":%" PRIu64
            ",\"targets_hz\":[%.3f,%.3f,%.3f,%.3f],"
            "\"target_level_dbfs\":[%.3f,%.3f,%.3f,%.3f],\"rms_dbfs\":%.3f}",
            measurement.timestamp_ms,
            (double)current_targets_hz[0],
            (double)current_targets_hz[1],
            (double)current_targets_hz[2],
            (double)current_targets_hz[3],
            (double)measurement.target_level_dbfs[0],
            (double)measurement.target_level_dbfs[1],
            (double)measurement.target_level_dbfs[2],
            (double)measurement.target_level_dbfs[3],
            (double)measurement.overall_rms_dbfs);
    } else {
        offset += snprintf(buffer + offset, capacity - (size_t)offset, "null");
    }

    offset += snprintf(buffer + offset, capacity - (size_t)offset, "}");
    return (offset > 0 && (size_t)offset < capacity) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static esp_err_t state_handler(httpd_req_t *req)
{
    if (append_state_json(s_json_buffer, sizeof(s_json_buffer)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, s_json_buffer, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t append_spectrum_json(char *buffer, size_t capacity)
{
    size_t bin_count = 0U;
    uint64_t timestamp_ms = 0U;
    if (audio_processing_copy_latest_spectrum(
            s_spectrum_dbfs, AUDIO_PROCESSING_SPECTRUM_BIN_COUNT, &bin_count,
            &timestamp_ms) != ESP_OK) {
        const int written = snprintf(buffer, capacity, "null");
        return written > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const double bin_width_hz =
        (double)AUDIO_PROCESSING_SAMPLE_RATE_HZ / AUDIO_PROCESSING_FFT_SIZE;
    int offset = snprintf(buffer, capacity,
                          "{\"timestamp_ms\":%" PRIu64
                          ",\"bin_width_hz\":%.6f,\"bin_count\":%u,\"dbfs\":[",
                          timestamp_ms, bin_width_hz, (unsigned)bin_count);
    if (offset < 0 || (size_t)offset >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t bin = 0; bin < bin_count; ++bin) {
        offset += snprintf(buffer + offset, capacity - (size_t)offset,
                           "%s%.3f", bin == 0U ? "" : ",",
                           (double)s_spectrum_dbfs[bin]);
        if (offset < 0 || (size_t)offset >= capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    offset += snprintf(buffer + offset, capacity - (size_t)offset, "]}");
    return (offset > 0 && (size_t)offset < capacity) ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static esp_err_t spectrum_handler(httpd_req_t *req)
{
    if (append_spectrum_json(s_json_buffer, sizeof(s_json_buffer)) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, s_json_buffer, HTTPD_RESP_USE_STRLEN);
}

/**
 * POST /target?id=<1-4>&hz=<float>
 *
 * Retargets which frequency band N (1 = the same index as cavity N) the
 * microphone measures — i.e. "block this frequency instead." Forwards
 * straight to audio_processing_set_target_frequency(), which does its own
 * 0..Nyquist range validation; this never moves a servo by itself.
 */
static esp_err_t target_handler(httpd_req_t *req)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing id and hz query parameters");
        return ESP_FAIL;
    }

    char id_str[8];
    char hz_str[16];
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK ||
        httpd_query_key_value(query, "hz", hz_str, sizeof(hz_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing id and hz query parameters");
        return ESP_FAIL;
    }

    char *id_end = NULL;
    char *hz_end = NULL;
    const int target_number = (int)strtol(id_str, &id_end, 10);
    const float requested_hz = strtof(hz_str, &hz_end);
    if (id_end == id_str || *id_end != '\0' || hz_end == hz_str ||
        *hz_end != '\0' || target_number < 1 ||
        target_number > (int)WIFI_STATUS_SERVER_TARGET_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "id must be 1-4 and hz must be a number");
        return ESP_FAIL;
    }

    const esp_err_t error = audio_processing_set_target_frequency(
        (size_t)(target_number - 1), requested_hz);
    if (error == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Requested frequency/bandwidth is outside 0..Nyquist");
        return ESP_FAIL;
    }
    if (error != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    float applied_hz = requested_hz;
    (void)audio_processing_get_target_frequency((size_t)(target_number - 1),
                                                &applied_hz);

    char body[64];
    const int written = snprintf(body, sizeof(body), "{\"id\":%d,\"hz\":%.3f}",
                                 target_number, (double)applied_hz);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, written);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG,
                        "Failed to start HTTP server");

    const httpd_uri_t state_uri = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = state_handler,
    };
    const httpd_uri_t spectrum_uri = {
        .uri = "/spectrum",
        .method = HTTP_GET,
        .handler = spectrum_handler,
    };
    const httpd_uri_t target_uri = {
        .uri = "/target",
        .method = HTTP_POST,
        .handler = target_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &state_uri), TAG,
                        "Failed to register /state handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &spectrum_uri),
                        TAG, "Failed to register /spectrum handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &target_uri), TAG,
                        "Failed to register /target handler");
    return ESP_OK;
}

esp_err_t wifi_status_server_init(const wifi_status_server_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Configuration must not be NULL");
    s_config = *config;

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "Failed to init NVS");
    ESP_RETURN_ON_ERROR(start_access_point(), TAG,
                        "Failed to start access point");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG,
                        "Failed to start HTTP server");
    return ESP_OK;
}
