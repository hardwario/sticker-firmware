/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STICKER_DRIVERS_SIM_SIM_H_
#define STICKER_DRIVERS_SIM_SIM_H_

/* STICKER simulator: what the emulated peripherals report. Set from the `sim`
 * shell command (sim_shell.c) or by a test harness through it. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADC pin voltage of one channel, mV (sim_adc.c). */
void sim_adc_set_mv(uint8_t channel, int32_t mv);
int32_t sim_adc_get_mv(uint8_t channel);

/* SHT4x reading; present=false makes the sensor stop answering (emul_sht4x.c). */
void sim_sht4x_set(float temperature, float humidity);
void sim_sht4x_set_present(bool present);
void sim_sht4x_get(float *temperature, float *humidity, bool *present);

/* OPT3001 illuminance in lux (emul_opt3001.c). */
void sim_opt3001_set(float lux);
void sim_opt3001_set_present(bool present);
void sim_opt3001_get(float *lux, bool *present);

/* SX1262 emulator state (emul_sx126x.c); mode = status-byte chip mode. */
void sim_sx126x_status(uint32_t *freq_hz, uint8_t *mode, uint32_t *tx, uint32_t *rx, bool *peer);

/* ST25DV emulator (emul_st25dv.c): RF field on/off and the chip state. */
void sim_st25dv_field(bool on);
void sim_st25dv_status(bool *field, bool *vcc, uint8_t *mb_ctrl, uint8_t *gpo_dyn,
		       uint32_t *rf_puts, uint32_t *host_puts);

#ifdef __cplusplus
}
#endif

#endif /* STICKER_DRIVERS_SIM_SIM_H_ */
