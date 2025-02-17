#include "app_alarm.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_wdog.h"
#include "app_tester.h"  // Include the new header file

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

void cmd_led_cycle(void)
{
    app_led_set(APP_LED_CHANNEL_R, 1);
    k_sleep(K_MSEC(500));
    app_led_set(APP_LED_CHANNEL_R, 0);
    //k_sleep(K_MSEC(250));
    app_led_set(APP_LED_CHANNEL_Y, 1);
    k_sleep(K_MSEC(500));
    app_led_set(APP_LED_CHANNEL_Y, 0);
    //k_sleep(K_MSEC(250));
    app_led_set(APP_LED_CHANNEL_G, 1);
    k_sleep(K_MSEC(1500));
    app_led_set(APP_LED_CHANNEL_G, 0);
}

//SYS_INIT(init, APPLICATION, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_test,
    SHELL_CMD_ARG(led_cycle, NULL, "Led cycle", cmd_led_cycle, 1, 0),
    SHELL_SUBCMD_SET_END
    );

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands", NULL);