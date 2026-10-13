/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_analog.h"
#include "app_led.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

/* Standard includes */
#include <errno.h>
#include <math.h>

LOG_MODULE_REGISTER(app_battery, LOG_LEVEL_DBG);

#define ADC_CHANNEL_BATT 2

#define R1_KOHM 560
#define R2_KOHM 100

static const struct device *m_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

/* Last successfully measured voltage, for callers that must not do a live ADC
 * read from a context where it can hang (#340 M9) - e.g. LoRaWAN DevStatusReq,
 * which runs on the system workqueue and would otherwise race the periodic
 * sensor sampler's own app_battery_measure() call on the same ADC. Own lock so
 * app_battery.c stays self-contained (no dependency on app_sensor.c). */
static float m_last_voltage = NAN;
static K_MUTEX_DEFINE(m_last_voltage_lock);

int app_battery_init(void)
{
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* The ADC driver boots ADC1 running; park it until the first read
	 * (app_analog_read_mv() resumes and sets the channel up per read). */
#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	return 0;
}

int app_battery_measure(float *voltage)
{
	int32_t mv;

	int ret = app_analog_read_mv(ADC_CHANNEL_BATT, &mv);
	if (ret) {
		return ret;
	}

	if (voltage) {
		*voltage = mv / 1000.f;
		*voltage = *voltage / R2_KOHM * (R1_KOHM + R2_KOHM);
		LOG_DBG("Battery voltage: %s%d.%02d V", APP_FP2(*voltage));

		k_mutex_lock(&m_last_voltage_lock, K_FOREVER);
		m_last_voltage = *voltage;
		k_mutex_unlock(&m_last_voltage_lock);
	}

	return 0;
}

float app_battery_last_sample(void)
{
	float v;

	k_mutex_lock(&m_last_voltage_lock, K_FOREVER);
	v = m_last_voltage;
	k_mutex_unlock(&m_last_voltage_lock);

	return v;
}
