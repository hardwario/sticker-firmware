/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MACHINE_PROBE_H_
#define APP_MACHINE_PROBE_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_machine_probe_scan(void);
int app_machine_probe_get_count(void);
int app_machine_probe_read_thermometer(int index, uint64_t *serial_number, float *temperature);
int app_machine_probe_read_hygrometer(int index, uint64_t *serial_number, float *temperature,
				      float *humidity);
int app_machine_probe_read_hygrometer_serial(int index, uint64_t *serial_number,
					     uint32_t *sht_serial);
int app_machine_probe_read_lux_meter(int index, uint64_t *serial_number, float *illuminance);
int app_machine_probe_read_magnetometer(int index, uint64_t *serial_number, float *magnetic_field);
int app_machine_probe_read_accelerometer(int index, uint64_t *serial_number, float *accel_x,
					 float *accel_y, float *accel_z, int *orientation);
int app_machine_probe_enable_tilt_alert(int index, uint64_t *serial_number, int threshold,
					int duration);
int app_machine_probe_disable_tilt_alert(int index, uint64_t *serial_number);
int app_machine_probe_get_tilt_alert(int index, uint64_t *serial_number, bool *is_active);

#ifdef __cplusplus
}
#endif

#endif /* APP_MACHINE_PROBE_H_ */
