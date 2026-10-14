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

/* MPL3115A2 pressure (hPa; altitude derives from it) and temperature
 * (emul_mpl3115a2.c). */
void sim_mpl3115a2_set(float pressure_hpa, float temperature);
void sim_mpl3115a2_set_present(bool present);
void sim_mpl3115a2_get(float *pressure_hpa, float *temperature, bool *present);

/* LIS2DH12 acceleration in m/s^2; a change is evaluated against the armed
 * interrupt generators. events = INT1/INT2 events so far (emul_lis2dh.c). */
void sim_lis2dh_set(float x, float y, float z);
void sim_lis2dh_set_present(bool present);
void sim_lis2dh_get(float accel[3], bool *present, uint32_t events[2]);

/* DS18B20 thermometers on the DS2484 1-Wire bus (emul_ds2484.c); index -1 in
 * set_present is the bridge itself. */
#define SIM_W1_SENSORS 4
void sim_w1_set(int index, float temperature);
void sim_w1_set_present(int index, bool present);
void sim_w1_get(int index, float *temperature, bool *present, uint8_t rom[8]);
void sim_w1_bus(bool *present, uint32_t *resets);

/* Drive an input of a hardwario,sim-gpio port (sim_gpio.c). */
struct device;
int sim_gpio_input_set(const struct device *port, uint8_t pin, int level);

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
