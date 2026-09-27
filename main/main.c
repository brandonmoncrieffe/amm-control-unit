#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
#include "servo_control.h"
#include "wifi_status_server.h"

static const char *TAG = "amm_control_unit";

static const servo_control_config_t SERVO_CONFIG = {
    .servos = {
        SERVO_CALIBRATION_DEFAULT(36),
        SERVO_CALIBRATION_DEFAULT(37),
        SERVO_CALIBRATION_DEFAULT(38),
        SERVO_CALIBRATION_DEFAULT(39),
    },
};

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
