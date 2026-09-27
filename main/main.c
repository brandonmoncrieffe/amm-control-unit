#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_processing.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linear_actuator.h"
#include "servo_control.h"
#include "wifi_status_server.h"

static const char *TAG = "amm_control_unit";

static const servo_control_config_t SERVO_CONFIG = {
    .servos = {
        SERVO_CALIBRATION_DEFAULT(36),
        {
            .gpio_num = 37,
            .min_pulse_us = SERVO_DEFAULT_MIN_PULSE_US,
            .max_pulse_us = SERVO_DEFAULT_MAX_PULSE_US,
            .center_offset_us = SERVO_DEFAULT_CENTER_OFFSET_US,
            .min_angle_deg = SERVO_DEFAULT_MIN_ANGLE_DEG,
            .max_angle_deg = 160,
            .initial_angle_deg = 0,
        },
        SERVO_CALIBRATION_DEFAULT(38),
        SERVO_CALIBRATION_DEFAULT(39),
    },
};

#define DEMO_SERVO_1_LENGTH_MM 23.0f
#define DEMO_SERVO_2_LENGTH_MM 6.5f
#define DEMO_ORIGINAL_LENGTH_MM 0.0f
#define DEMO_MEASUREMENT_COUNT 8U
#define DEMO_SETTLE_TIME_MS 1500U
#define DEMO_MEASUREMENT_POLL_MS 20U
#define DEMO_MEASUREMENT_MAX_POLLS 250U
#define DEMO_TARGET_COUNT 2U

static const audio_processing_config_t AUDIO_CONFIG = {
    .bclk_gpio = GPIO_NUM_10,
    .ws_gpio = GPIO_NUM_11,
    .data_gpio = GPIO_NUM_12,
    .target_frequency_hz = {73.0f, 145.0f, 213.0f, 395.0f},
    .target_half_bandwidth_hz = 20.0f,
};

static atomic_bool s_stream_enabled = ATOMIC_VAR_INIT(false);
static SemaphoreHandle_t s_serial_output_mutex;
static QueueHandle_t s_stream_queue;
static float s_spectrum_snapshot[AUDIO_PROCESSING_SPECTRUM_BIN_COUNT];
static bool s_demo_baseline_valid;
static float s_demo_baseline_dbfs[DEMO_TARGET_COUNT];

static void print_audio_config_record(void)
{
    printf("AUDIO_CONFIG,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f\n",
           AUDIO_PROCESSING_SAMPLE_RATE_HZ, AUDIO_PROCESSING_FFT_SIZE,
           (double)AUDIO_CONFIG.target_half_bandwidth_hz,
           (double)AUDIO_CONFIG.target_frequency_hz[0],
           (double)AUDIO_CONFIG.target_frequency_hz[1],
           (double)AUDIO_CONFIG.target_frequency_hz[2],
           (double)AUDIO_CONFIG.target_frequency_hz[3]);
}

static void print_measurement_record(const audio_measurement_t *measurement)
{
    printf("SPECTRUM,%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f\n",
           measurement->timestamp_ms,
           (double)measurement->target_level_dbfs[0],
           (double)measurement->target_level_dbfs[1],
           (double)measurement->target_level_dbfs[2],
           (double)measurement->target_level_dbfs[3],
           (double)measurement->overall_rms_dbfs);
}

static void audio_measurement_callback(
    const audio_measurement_t *measurement, void *context)
{
    (void)context;
    xQueueOverwrite(s_stream_queue, measurement);
}

static void serial_stream_task(void *context)
{
    (void)context;
    audio_measurement_t measurement;

    while (true) {
        if (xQueueReceive(s_stream_queue, &measurement, portMAX_DELAY) ==
                pdTRUE &&
            atomic_load(&s_stream_enabled) &&
            xSemaphoreTake(s_serial_output_mutex, 0) == pdTRUE) {
            print_measurement_record(&measurement);
            xSemaphoreGive(s_serial_output_mutex);
        }
    }
}

static bool parse_integer(const char *text, int *value)
{
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || parsed < INT_MIN ||
        parsed > INT_MAX) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

static bool parse_float(const char *text, float *value)
{
    char *end = NULL;
    errno = 0;
    const float parsed = strtof(text, &end);

    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }

    *value = parsed;
    return true;
}

static int set_command(int argc, char **argv)
{
    int servo_number;
    int angle;

    if (argc != 3 || !parse_integer(argv[1], &servo_number) ||
        !parse_integer(argv[2], &angle) || servo_number < 1 ||
        servo_number > SERVO_COUNT) {
        printf("Usage: set <servo 1-4> <angle>\n");
        return 1;
    }

    const esp_err_t error =
        servo_set_angle((servo_id_t)(servo_number - 1), angle);
    if (error != ESP_OK) {
        printf("Failed to set servo %d: %s\n", servo_number,
               esp_err_to_name(error));
        return 1;
    }

    return 0;
}

static int all_command(int argc, char **argv)
{
    int angle;

    if (argc != 2 || !parse_integer(argv[1], &angle)) {
        printf("Usage: all <angle>\n");
        return 1;
    }

    const esp_err_t error = servo_set_all_angles(angle);
    if (error != ESP_OK) {
        printf("Failed to set all servos: %s\n", esp_err_to_name(error));
        return 1;
    }

    return 0;
}

static int center_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: center\n");
        return 1;
    }

    const esp_err_t error = servo_center_all();
    if (error != ESP_OK) {
        printf("Failed to center servos: %s\n", esp_err_to_name(error));
        return 1;
    }

    return 0;
}

static int status_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: status\n");
        return 1;
    }

    for (servo_id_t servo_id = SERVO_ID_1; servo_id < SERVO_ID_COUNT;
         servo_id = (servo_id_t)(servo_id + 1)) {
        int angle;
        const esp_err_t error = servo_get_commanded_angle(servo_id, &angle);
        if (error != ESP_OK) {
            printf("Failed to read servo %d status: %s\n", (int)servo_id + 1,
                   esp_err_to_name(error));
            return 1;
        }
        printf("Servo %d (GPIO %d): %d degrees\n", (int)servo_id + 1,
               SERVO_CONFIG.servos[servo_id].gpio_num, angle);
    }

    return 0;
}

static int length_command(int argc, char **argv)
{
    int servo_number;
    float requested_length_mm;

    if (argc != 3 || !parse_integer(argv[1], &servo_number) ||
        !parse_float(argv[2], &requested_length_mm) || servo_number < 1 ||
        servo_number > SERVO_COUNT) {
        printf("Usage: length <servo 1-4> <millimetres>\n");
        return 1;
    }

    const servo_id_t servo_id = (servo_id_t)(servo_number - 1);
    if (!linear_actuator_is_calibrated(servo_id)) {
        printf("Servo %d has no embedded length calibration; add "
               "calibration/servo_%d.csv, rebuild, and flash\n",
               servo_number, servo_number);
        return 1;
    }

    linear_actuator_command_result_t result;
    const esp_err_t error = linear_actuator_set_length(
        servo_id, requested_length_mm, &result);
    if (error != ESP_OK) {
        printf("Failed to set servo %d length: %s\n", servo_number,
               esp_err_to_name(error));
        return 1;
    }

    printf("Servo %d length: requested=%.3f mm, clamped=%.3f mm, "
           "angle=%d deg\n",
           servo_number, (double)result.requested_length_mm,
           (double)result.clamped_length_mm, result.commanded_angle_deg);
    return 0;
}

static int length_status_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: lengthstatus\n");
        return 1;
    }

    for (servo_id_t servo_id = SERVO_ID_1; servo_id < SERVO_ID_COUNT;
         servo_id = (servo_id_t)(servo_id + 1)) {
        const int servo_number = (int)servo_id + 1;
        if (!linear_actuator_is_calibrated(servo_id)) {
            printf("Servo %d: no embedded length calibration\n",
                   servo_number);
            continue;
        }

        int angle_deg = 0;
        float length_mm = 0.0f;
        float minimum_mm = 0.0f;
        float maximum_mm = 0.0f;
        const esp_err_t angle_error =
            servo_get_commanded_angle(servo_id, &angle_deg);
        const esp_err_t length_error =
            linear_actuator_get_estimated_length(servo_id, &length_mm);
        const esp_err_t range_error = linear_actuator_get_range(
            servo_id, &minimum_mm, &maximum_mm);
        if (angle_error != ESP_OK || range_error != ESP_OK) {
            printf("Failed to read servo %d length status\n", servo_number);
            return 1;
        }
        if (length_error == ESP_ERR_INVALID_STATE) {
            printf("Servo %d: angle=%d deg is outside its calibrated range "
                   "%.3f..%.3f mm\n",
                   servo_number, angle_deg, (double)minimum_mm,
                   (double)maximum_mm);
            continue;
        }
        if (length_error != ESP_OK) {
            printf("Failed to estimate servo %d length: %s\n", servo_number,
                   esp_err_to_name(length_error));
            return 1;
        }

        printf("Servo %d: estimated=%.3f mm, angle=%d deg, range=%.3f..%.3f "
               "mm\n",
               servo_number, (double)length_mm, angle_deg,
               (double)minimum_mm, (double)maximum_mm);
    }
    return 0;
}

static esp_err_t set_demo_lengths(float servo_1_length_mm,
                                  float servo_2_length_mm,
                                  linear_actuator_command_result_t *servo_1,
                                  linear_actuator_command_result_t *servo_2)
{
    if (!linear_actuator_is_calibrated(SERVO_ID_1) ||
        !linear_actuator_is_calibrated(SERVO_ID_2)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t error = linear_actuator_set_length(
        SERVO_ID_1, servo_1_length_mm, servo_1);
    if (error != ESP_OK) {
        return error;
    }
    return linear_actuator_set_length(SERVO_ID_2, servo_2_length_mm, servo_2);
}

static esp_err_t collect_demo_average(float averaged_dbfs[DEMO_TARGET_COUNT])
{
    double power_sum[DEMO_TARGET_COUNT] = {0.0, 0.0};
    uint64_t previous_timestamp_ms = UINT64_MAX;
    size_t collected = 0U;

    for (size_t poll = 0U;
         poll < DEMO_MEASUREMENT_MAX_POLLS &&
         collected < DEMO_MEASUREMENT_COUNT;
         ++poll) {
        audio_measurement_t measurement;
        if (audio_processing_get_latest(&measurement) == ESP_OK &&
            measurement.timestamp_ms != previous_timestamp_ms) {
            previous_timestamp_ms = measurement.timestamp_ms;
            for (size_t target = 0U; target < DEMO_TARGET_COUNT; ++target) {
                power_sum[target] += pow(10.0,
                                         measurement.target_level_dbfs[target] /
                                             10.0);
            }
            ++collected;
        }
        if (collected < DEMO_MEASUREMENT_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(DEMO_MEASUREMENT_POLL_MS));
        }
    }

    if (collected != DEMO_MEASUREMENT_COUNT) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t target = 0U; target < DEMO_TARGET_COUNT; ++target) {
        averaged_dbfs[target] =
            (float)(10.0 * log10(power_sum[target] / (double)collected));
    }
    return ESP_OK;
}

static int demo_command(int argc, char **argv)
{
    int stage;
    if (argc != 2 || !parse_integer(argv[1], &stage) ||
        (stage != 1 && stage != 2)) {
        printf("Usage: demo <1|2>\n");
        return 1;
    }
    if (!linear_actuator_is_calibrated(SERVO_ID_1) ||
        !linear_actuator_is_calibrated(SERVO_ID_2)) {
        printf("Demo requires embedded length calibration for servos 1 and 2\n");
        return 1;
    }
    if (stage == 2 && !s_demo_baseline_valid) {
        printf("Run 'demo 1' first to capture the original-position baseline\n");
        return 1;
    }

    linear_actuator_command_result_t servo_1_result;
    linear_actuator_command_result_t servo_2_result;
    const float servo_1_length =
        stage == 1 ? DEMO_ORIGINAL_LENGTH_MM : DEMO_SERVO_1_LENGTH_MM;
    const float servo_2_length =
        stage == 1 ? DEMO_ORIGINAL_LENGTH_MM : DEMO_SERVO_2_LENGTH_MM;
    if (stage == 1) {
        s_demo_baseline_valid = false;
    }

    esp_err_t error = set_demo_lengths(
        servo_1_length, servo_2_length, &servo_1_result, &servo_2_result);
    if (error != ESP_OK) {
        printf("Demo %d failed while positioning servos: %s\n", stage,
               esp_err_to_name(error));
        return 1;
    }

    printf("Demo %d positions: servo 1 %.3f mm (%d deg), servo 2 %.3f mm "
           "(%d deg); settling for %.1f seconds\n",
           stage, (double)servo_1_result.clamped_length_mm,
           servo_1_result.commanded_angle_deg,
           (double)servo_2_result.clamped_length_mm,
           servo_2_result.commanded_angle_deg,
           (double)DEMO_SETTLE_TIME_MS / 1000.0);
    vTaskDelay(pdMS_TO_TICKS(DEMO_SETTLE_TIME_MS));

    float averaged_dbfs[DEMO_TARGET_COUNT];
    error = collect_demo_average(averaged_dbfs);
    if (error != ESP_OK) {
        printf("Demo %d could not collect %u fresh audio measurements: %s\n",
               stage, DEMO_MEASUREMENT_COUNT, esp_err_to_name(error));
        return 1;
    }

    if (stage == 1) {
        memcpy(s_demo_baseline_dbfs, averaged_dbfs,
               sizeof(s_demo_baseline_dbfs));
        s_demo_baseline_valid = true;
        printf("Demo 1 baseline (%u-sample power average): %.0f Hz=%.3f "
               "dBFS, %.0f Hz=%.3f dBFS\n",
               DEMO_MEASUREMENT_COUNT,
               (double)AUDIO_CONFIG.target_frequency_hz[0],
               (double)s_demo_baseline_dbfs[0],
               (double)AUDIO_CONFIG.target_frequency_hz[1],
               (double)s_demo_baseline_dbfs[1]);
        return 0;
    }

    printf("Demo 2 comparison (%u-sample power average; positive drop means "
           "suppression):\n",
           DEMO_MEASUREMENT_COUNT);
    for (size_t target = 0U; target < DEMO_TARGET_COUNT; ++target) {
        const float drop_db =
            s_demo_baseline_dbfs[target] - averaged_dbfs[target];
        printf("  %.0f Hz: baseline=%.3f dBFS, demo=%.3f dBFS, "
               "drop=%.3f dB\n",
               (double)AUDIO_CONFIG.target_frequency_hz[target],
               (double)s_demo_baseline_dbfs[target],
               (double)averaged_dbfs[target], (double)drop_db);
    }
    return 0;
}

static int stream_command(int argc, char **argv)
{
    if (argc != 2 ||
        (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        printf("Usage: stream <on|off>\n");
        return 1;
    }

    const bool enable = strcmp(argv[1], "on") == 0;
    xSemaphoreTake(s_serial_output_mutex, portMAX_DELAY);
    if (enable) {
        print_audio_config_record();
        atomic_store(&s_stream_enabled, true);
    } else {
        atomic_store(&s_stream_enabled, false);
        printf("Audio stream off\n");
    }
    xSemaphoreGive(s_serial_output_mutex);
    return 0;
}

static int level_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: level\n");
        return 1;
    }

    audio_measurement_t measurement;
    if (audio_processing_get_latest(&measurement) != ESP_OK) {
        printf("Audio measurement not ready\n");
        return 1;
    }

    xSemaphoreTake(s_serial_output_mutex, portMAX_DELAY);
    print_measurement_record(&measurement);
    xSemaphoreGive(s_serial_output_mutex);
    return 0;
}

static int spectrum_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: spectrum\n");
        return 1;
    }

    size_t bin_count = 0U;
    uint64_t timestamp_ms = 0U;
    const esp_err_t error = audio_processing_copy_latest_spectrum(
        s_spectrum_snapshot, AUDIO_PROCESSING_SPECTRUM_BIN_COUNT, &bin_count,
        &timestamp_ms);
    if (error != ESP_OK) {
        printf("Audio measurement not ready\n");
        return 1;
    }

    const double bin_width_hz = (double)AUDIO_PROCESSING_SAMPLE_RATE_HZ /
                                AUDIO_PROCESSING_FFT_SIZE;
    xSemaphoreTake(s_serial_output_mutex, portMAX_DELAY);
    printf("FFT_BEGIN,%" PRIu64 ",%u,%u,%.6f,%u\n", timestamp_ms,
           AUDIO_PROCESSING_SAMPLE_RATE_HZ, AUDIO_PROCESSING_FFT_SIZE,
           bin_width_hz, (unsigned)bin_count);
    for (size_t start = 0U; start < bin_count; start += 32U) {
        const size_t end =
            (start + 32U < bin_count) ? start + 32U : bin_count;
        printf("FFT,%u", (unsigned)start);
        for (size_t bin = start; bin < end; ++bin) {
            printf(",%.3f", (double)s_spectrum_snapshot[bin]);
        }
        printf("\n");
    }
    printf("FFT_END,%" PRIu64 "\n", timestamp_ms);
    xSemaphoreGive(s_serial_output_mutex);
    return 0;
}

static void register_console_commands(void)
{
    const esp_console_cmd_t commands[] = {
        {
            .command = "set",
            .help = "Set one servo angle; values are clamped to its safe range",
            .hint = "<servo 1-4> <angle>",
            .func = &set_command,
        },
        {
            .command = "all",
            .help = "Set all servo angles using per-servo calibration",
            .hint = "<angle>",
            .func = &all_command,
        },
        {
            .command = "center",
            .help = "Command all servos to 90 degrees",
            .func = &center_command,
        },
        {
            .command = "status",
            .help = "Show the last commanded angle for every servo",
            .func = &status_command,
        },
        {
            .command = "length",
            .help = "Set one calibrated actuator length in millimetres",
            .hint = "<servo 1-4> <millimetres>",
            .func = &length_command,
        },
        {
            .command = "lengthstatus",
            .help = "Show estimated lengths and embedded calibration ranges",
            .func = &length_status_command,
        },
        {
            .command = "demo",
            .help = "Capture original baseline or compare demo positions",
            .hint = "<1|2>",
            .func = &demo_command,
        },
        {
            .command = "stream",
            .help = "Enable or disable continuous target-frequency data",
            .hint = "<on|off>",
            .func = &stream_command,
        },
        {
            .command = "level",
            .help = "Print the latest target-frequency and RMS levels",
            .func = &level_command,
        },
        {
            .command = "spectrum",
            .help = "Print the latest complete FFT in chunked CSV records",
            .func = &spectrum_command,
        },
    };

    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
         ++index) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[index]));
    }
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "servo>";
    repl_config.max_cmdline_length = 128;

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hardware_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(
        &hardware_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || \
    defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hardware_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(
        esp_console_new_repl_uart(&hardware_config, &repl_config, &repl));
#else
#error "Configure either USB Serial/JTAG or UART as the primary console"
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "AMM control unit firmware started");
    s_serial_output_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_serial_output_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    s_stream_queue = xQueueCreate(1U, sizeof(audio_measurement_t));
    ESP_ERROR_CHECK(s_stream_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(serial_stream_task, "serial_stream", 3072U,
                                NULL, 4U, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(servo_init(&SERVO_CONFIG));
    ESP_ERROR_CHECK(audio_processing_init(&AUDIO_CONFIG,
                                          audio_measurement_callback, NULL));

    wifi_status_server_config_t wifi_status_config;
    for (size_t index = 0; index < WIFI_STATUS_SERVER_SERVO_COUNT; ++index) {
        wifi_status_config.servo_gpio[index] =
            SERVO_CONFIG.servos[index].gpio_num;
    }
    memcpy(wifi_status_config.target_frequency_hz,
           AUDIO_CONFIG.target_frequency_hz,
           sizeof(wifi_status_config.target_frequency_hz));
    ESP_ERROR_CHECK(wifi_status_server_init(&wifi_status_config));

    register_console_commands();
    start_console();
}
