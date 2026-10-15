/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ANALOG_H_
#define APP_ANALOG_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADC1 access shared by the battery monitor (app_battery.c) and the analog
 * voltage on the external inputs GP_A (PB4, ADC1_IN3) / GP_B (PA11, ADC1_IN7),
 * #396. */

/* One-shot 12-bit read of ADC1 channel `channel_id` in millivolts at the pin.
 * Resumes ADC1, sets the channel up, samples and suspends it again; every
 * caller is serialized on one lock, so concurrent callers never interleave
 * their RESUME/SUSPEND pairs. */
int app_analog_read_mv(uint8_t channel_id, int32_t *mv);

/* Put the GP_A / GP_B pins whose cap_gpio_input_a / _b is analog into analog
 * mode. Call from app_sensor_init() after the pin use is resolved (#498) and
 * after app_input_init(), which configures both pins as digital inputs. */
int app_analog_init(void);

/* Input-referred voltage at the connector (V), divider-corrected (x34, see
 * app_analog.c). *voltage is left untouched on error. */
int app_analog_measure_a(float *voltage);
int app_analog_measure_b(float *voltage);

#ifdef __cplusplus
}
#endif

#endif /* APP_ANALOG_H_ */
