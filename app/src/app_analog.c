/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_analog.h"
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

LOG_MODULE_REGISTER(app_analog, LOG_LEVEL_DBG);

/* GP_A = PB4 = ADC1_IN3, GP_B = PA11 = ADC1_IN7 (schematic + #396 analysis). No
 * external divider on either pin, so the measured range is 0..VDD directly. */
#define ADC_CHANNEL_A 3
#define ADC_CHANNEL_B 7

static const struct device *m_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

static const struct adc_channel_cfg m_channel_cfg_a = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_MAX,
	.channel_id = ADC_CHANNEL_A,
	.differential = 0,
};

static const struct adc_channel_cfg m_channel_cfg_b = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_MAX,
	.channel_id = ADC_CHANNEL_B,
	.differential = 0,
};

int app_analog_init(void)
{
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup(m_dev, &m_channel_cfg_a);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_channel_setup", ret);
		return ret;
	}

	ret = adc_channel_setup(m_dev, &m_channel_cfg_b);
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

static int measure_channel(uint8_t channel_id, float *voltage)
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
		.channels = BIT(channel_id),
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
	ret = adc_raw_to_millivolts(adc_ref_internal(m_dev), ADC_GAIN_1, seq.resolution, &voltage_);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_raw_to_millivolts", ret);
		res = ret;
		goto suspend;
	}

	LOG_DBG("Channel %u voltage: %d mV (raw: %d)", channel_id, voltage_, sample);

	if (voltage) {
		*voltage = voltage_ / 1000.f;
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

int app_analog_measure_a(float *voltage)
{
	return measure_channel(ADC_CHANNEL_A, voltage);
}

int app_analog_measure_b(float *voltage)
{
	return measure_channel(ADC_CHANNEL_B, voltage);
}
