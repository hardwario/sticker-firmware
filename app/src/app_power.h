/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enter deep sleep: stop LoRaWAN + sensor activity, turn the LEDs off, then
 * power the STM32WL down via sys_poweroff() (Shutdown mode). Does NOT return;
 * wake is by NRST / power-cycle (a clean boot — NVS identity and LoRaWAN keys
 * survive in flash). Requires CONFIG_POWEROFF. */
void app_power_suspend(void);

/* Power the STM32WL down to Shutdown right away, without the module teardown
 * of app_power_suspend() (for a boot that has not started them). Does NOT
 * return; wake is by NRST / power-cycle. */
void app_power_off(void);

/* #496: call first thing at boot, before anything loads the cell. True when
 * the battery is below CONFIG_APP_BATTERY_BOOT_MIN_MV: the STICKER must not
 * start (app_power_low_battery_off()). False when it is fine, cannot be
 * measured, or without CONFIG_APP_BATTERY_BOOT_GATE. */
bool app_power_boot_battery_low(void);

/* #496: the low-battery "off" indication -- red blinks every 200 ms for 5 s,
 * then red for 1 s -- then app_power_off(). Takes the LED (app_led_hold()) and
 * feeds the IWDG. Does NOT return. */
void app_power_low_battery_off(void);

/* Debug idle watchdog. Call periodically from the main loop. After
 * CONFIG_APP_DEBUG_AUTOSUSPEND_S with no RTT/shell interaction it calls
 * app_power_suspend(). Compiled to a no-op unless CONFIG_FW_DEBUG and the
 * timeout is > 0 — Debug FW runs with CONFIG_PM=n (CPU never sleeps), so a
 * forgotten bench unit would otherwise drain its battery. */
void app_power_check_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_POWER_H_ */
