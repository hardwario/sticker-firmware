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

/* Enter user-initiated NFC standby (#156): stop LoRaWAN + sensor activity, turn
 * the LEDs off, arm the ST25DV GPO to wake on an RF field change, then enter
 * STM32WL Stop2 (Shutdown can't be woken by the GPO — it is on an EXTI pin, not
 * a WKUP pin). A phone tap (or NRST / power-cycle) wakes the MCU and the device
 * cold-reboots into normal operation. RAM-only: standby does not persist across
 * a reset. Returns only if the wake source could not be armed (aborts to normal
 * operation rather than entering an unwakeable sleep). */
void app_power_standby(void);

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
