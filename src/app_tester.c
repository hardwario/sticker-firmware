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
    k_sleep(K_MSEC(1000));
    app_led_set(APP_LED_CHANNEL_R, 0);
    k_sleep(K_MSEC(200));
    app_led_set(APP_LED_CHANNEL_Y, 1);
    k_sleep(K_MSEC(1000));
    app_led_set(APP_LED_CHANNEL_Y, 0);
    k_sleep(K_MSEC(200));
    app_led_set(APP_LED_CHANNEL_G, 1);
    k_sleep(K_MSEC(1000));
    app_led_set(APP_LED_CHANNEL_G, 0);
}

static void cmd_led_switch(const struct shell *shell, size_t argc, char **argv)
{
	enum app_led_channel channel;

	if (strcmp(argv[1], "red") == 0) {
		channel = APP_LED_CHANNEL_R;

	} else if (strcmp(argv[1], "green") == 0) {
		channel = APP_LED_CHANNEL_G;

	} else if (strcmp(argv[1], "yellow") == 0) {
		channel = APP_LED_CHANNEL_Y;

	} else {
		shell_error(shell, "invalid channel name");
		shell_help(shell);
        return;		
	}

	if (strcmp(argv[2], "on") == 0) {
		app_led_set(channel, true);

	} else if (strcmp(argv[2], "off") == 0) {
		app_led_set(channel, false);

	} else {
		shell_error(shell, "invalid command");
		shell_help(shell);
	}

}

static void cmd_print_voltage(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " voltage %.2f", (double)g_app_sensor_data.voltage);
}

static void cmd_print_orientation(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " orientation %d", (int)g_app_sensor_data.orientation);
}

static void cmd_print_temperature(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " temperature %.2f", (double)g_app_sensor_data.temperature);
}

static void cmd_print_humidity(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " humidity %.2f", (double)g_app_sensor_data.humidity);
}

static void cmd_print_illuminance(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " illuminance %.2f", (double)g_app_sensor_data.illuminance);
}

static void cmd_print_ext_temperature_1(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " ext_temperature_1 %.2f", (double)g_app_sensor_data.ext_temperature_1);
}

static void cmd_print_ext_temperature_2(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " ext_temperature_2 %.2f", (double)g_app_sensor_data.ext_temperature_2);
}

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
static void cmd_print_motion_count(const struct shell *shell)
{
    app_sensor_sample();
    shell_print(shell, SETTINGS_PFX " motion_count %d", (int)g_app_sensor_data.motion_count);
}
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */


//SYS_INIT(init, APPLICATION, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_test,
    SHELL_CMD_ARG(led_cycle, NULL, "Led cycle. R-Y-G", cmd_led_cycle, 1, 0),
    SHELL_CMD_ARG(led_switch, NULL, "Switch LED channel (format red|yellow|green on|off).",cmd_led_switch, 3, 0),
    SHELL_CMD_ARG(voltage, NULL, "Get voltage.", cmd_print_voltage, 1, 0),
    SHELL_CMD_ARG(orientation, NULL, "Get orientation.", cmd_print_orientation, 1, 0),
    SHELL_CMD_ARG(temperature, NULL, "Get temperature.", cmd_print_temperature, 1, 0),
    SHELL_CMD_ARG(humidity, NULL, "Get humidity.", cmd_print_humidity, 1, 0),
    SHELL_CMD_ARG(illuminance, NULL, "Get illuminance.", cmd_print_illuminance, 1, 0),
    SHELL_CMD_ARG(ext_temperature_1, NULL, "Get ext_temperature_1.", cmd_print_ext_temperature_1, 1, 0),
    SHELL_CMD_ARG(ext_temperature_2, NULL, "Get ext_temperature_2.", cmd_print_ext_temperature_2, 1, 0),
    #if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
    SHELL_CMD_ARG(motion, NULL, "Get count of motions", cmd_print_motion_count, 1, 0),
    #endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands", NULL);

#endif /* defined(CONFIG_SHELL) */