/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_led.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
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

static const struct adc_channel_cfg m_channel_cfg = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_MAX,
	.channel_id = ADC_CHANNEL_BATT,
	.differential = 0,
};

int app_battery_init(void)
{
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup(m_dev, &m_channel_cfg);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_channel_setup", ret);
		return ret;
	}

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
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_RESUME);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	int res = 0;
	int16_t sample;

	struct adc_sequence seq = {
		.channels = BIT(ADC_CHANNEL_BATT),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	ret = adc_read(m_dev, &seq);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_read", ret);
		res = ret;
		goto suspend;
	}

	int32_t voltage_ = sample;
	ret = adc_raw_to_millivolts(adc_ref_internal(m_dev), m_channel_cfg.gain, seq.resolution,
				    &voltage_);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_raw_to_millivolts", ret);
		res = ret;
		goto suspend;
	}

	LOG_DBG("ADC voltage: %d mV (raw: %d)", voltage_, sample);

	if (voltage) {
		*voltage = voltage_ / 1000.f;
		*voltage = *voltage / R2_KOHM * (R1_KOHM + R2_KOHM);
		LOG_DBG("Battery voltage: %s%d.%02d V", APP_FP2(*voltage));

		k_mutex_lock(&m_last_voltage_lock, K_FOREVER);
		m_last_voltage = *voltage;
		k_mutex_unlock(&m_last_voltage_lock);
	}

suspend:
#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		return res ? res : ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	return res;
}

float app_battery_last_sample(void)
{
	float v;

	k_mutex_lock(&m_last_voltage_lock, K_FOREVER);
	v = m_last_voltage;
	k_mutex_unlock(&m_last_voltage_lock);

	return v;
}
