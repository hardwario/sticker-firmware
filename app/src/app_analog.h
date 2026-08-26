/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ANALOG_H_
#define APP_ANALOG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Analog voltage measurement on the external inputs GP_A (PB4/ADC1_IN3) and
 * GP_B (PA11/ADC1_IN7), 12-bit (#396). An external 33k/1k divider on the JP11
 * front-end scales a 0..24 V input into the pin's 0..VDD range; the returned
 * voltage is divider-corrected to the input at the connector (x34, see
 * app_analog.c). Mirrors app_battery.c's ADC1 channel pattern (PM
 * resume/suspend around each one-shot read). Only call app_analog_init() when
 * at least one of
 * cap_analog_a/cap_analog_b is enabled — app_sensor_init() owns the mutual
 * exclusion with cap_input_a/b and cap_pir_detector/cap_buzzer, which all
 * share these same two physical pins (#90). */

/* Set up the ADC1 channels for GP_A/GP_B, then leave ADC1 suspended. */
int app_analog_init(void);

/* One-shot reads: resume ADC1, sample, suspend again. *voltage is left
 * untouched on error. */
int app_analog_measure_a(float *voltage);
int app_analog_measure_b(float *voltage);

#ifdef __cplusplus
}
#endif

#endif /* APP_ANALOG_H_ */
