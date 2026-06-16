/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Enter deep sleep: stop LoRaWAN + sensor activity, turn the LEDs off, then
 * power the STM32WL down via sys_poweroff() (Shutdown mode). Does NOT return;
 * wake is by NRST / power-cycle (a clean boot — NVS identity and LoRaWAN keys
 * survive in flash). Requires CONFIG_POWEROFF. */
void app_power_suspend(void);

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
