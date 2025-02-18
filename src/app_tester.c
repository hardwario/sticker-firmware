#include "app_alarm.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_wdog.h"
#include "app_tester.h"  // Include the new header file

/* Zephyr includes */
#include <zephyr/fs/fs.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_tester, LOG_LEVEL_DBG);

#define SETTINGS_PFX "tester"

//struct app_config g_app_config;

#if defined(CONFIG_SHELL)

void cmd_led_cycle(void)
{
    app_led_set(APP_LED_CHANNEL_R, 1);
    k_sleep(K_MSEC(800));
    app_led_set(APP_LED_CHANNEL_R, 0);
    app_led_set(APP_LED_CHANNEL_Y, 1);
    k_sleep(K_MSEC(800));
    app_led_set(APP_LED_CHANNEL_Y, 0);
    app_led_set(APP_LED_CHANNEL_G, 1);
    k_sleep(K_MSEC(800));
    app_led_set(APP_LED_CHANNEL_G, 0);
}

static void cmd_print_voltage(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " voltage %.2f", (double)g_app_sensor_data.voltage);
}

static void cmd_print_orientation(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " orientation %.2f", (double)g_app_sensor_data.orientation);
}

static void cmd_print_temperature(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " temperature %.2f", (double)g_app_sensor_data.temperature);
}

static void cmd_print_humidity(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " humidity %.2f", (double)g_app_sensor_data.humidity);
}

static void cmd_print_illuminance(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " illuminance %.2f", (double)g_app_sensor_data.illuminance);
}

static void cmd_print_ext_temperature_1(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " ext_temperature_1 %.2f", (double)g_app_sensor_data.ext_temperature_1);
}

static void cmd_print_ext_temperature_2(const struct shell *shell)
{
    shell_print(shell, SETTINGS_PFX " ext_temperature_2 %.2f", (double)g_app_sensor_data.ext_temperature_2);
}


//SYS_INIT(init, APPLICATION, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_test,
    SHELL_CMD_ARG(led_cycle, NULL, "Led cycle. R-Y-G", cmd_led_cycle, 1, 0),
    SHELL_CMD_ARG(voltage, NULL, "Get voltage.", cmd_print_voltage, 1, 0),
    SHELL_CMD_ARG(orientation, NULL, "Get orientation.", cmd_print_orientation, 1, 0),
    SHELL_CMD_ARG(temperature, NULL, "Get temperature.", cmd_print_temperature, 1, 0),
    SHELL_CMD_ARG(humidity, NULL, "Get humidity.", cmd_print_humidity, 1, 0),
    SHELL_CMD_ARG(illuminance, NULL, "Get illuminance.", cmd_print_illuminance, 1, 0),
    SHELL_CMD_ARG(ext_temperature_1, NULL, "Get ext_temperature_1.", cmd_print_ext_temperature_1, 1, 0),
    SHELL_CMD_ARG(ext_temperature_2, NULL, "Get ext_temperature_2.", cmd_print_ext_temperature_2, 1, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands", NULL);

#endif /* defined(CONFIG_SHELL) */