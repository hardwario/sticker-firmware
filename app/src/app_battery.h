/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BATTERY_H_
#define APP_BATTERY_H_

#ifdef __cplusplus
extern "C" {
#endif

int app_battery_init(void);
int app_battery_measure(float *voltage);

/* Last successfully measured voltage (NAN if none yet) - for callers that must
 * not do a live ADC read from a context where it can hang (#340 M9), e.g. a
 * LoRaWAN MAC callback running on the system workqueue. */
float app_battery_last_sample(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BATTERY_H_ */
