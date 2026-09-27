#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "servo_control.h"

static const char *TAG = "amm_control_unit";

static const servo_control_config_t SERVO_CONFIG = {
    .servos = {
        SERVO_CALIBRATION_DEFAULT(36),
        SERVO_CALIBRATION_DEFAULT(37),
        SERVO_CALIBRATION_DEFAULT(38),
        SERVO_CALIBRATION_DEFAULT(39),
    },
};

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
    ESP_ERROR_CHECK(servo_init(&SERVO_CONFIG));
    register_console_commands();
    start_console();
}
