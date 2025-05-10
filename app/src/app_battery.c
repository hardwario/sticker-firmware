/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_led.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_battery, LOG_LEVEL_DBG);

#define ADC_CHANNEL_BATT 2

#define R1_KOHM 560
#define R2_KOHM 100

static const struct device *m_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

static const struct adc_channel_cfg m_channel_cfg = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_MAX,
	.channel_id = ADC_CHANNEL_BATT,
	.differential = 0,
};

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
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	int16_t sample;

	struct adc_sequence seq = {
		.channels = BIT(ADC_CHANNEL_BATT),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	ret = adc_read(m_dev, &seq);
	if (ret) {
		LOG_ERR("Call `adc_read` failed: %d", ret);
		return ret;
	}

	int32_t voltage_ = sample;
	ret = adc_raw_to_millivolts(adc_ref_internal(m_dev), m_channel_cfg.gain, seq.resolution,
				    &voltage_);
	if (ret) {
		LOG_ERR("Call `adc_raw_to_millivolts` failed: %d", ret);
		return ret;
	}

	LOG_DBG("ADC voltage: %d mV (raw: %d)", voltage_, sample);

	if (voltage) {
		*voltage = voltage_ / 1000.f;
		*voltage = *voltage / R2_KOHM * (R1_KOHM + R2_KOHM);
	}

	LOG_DBG("Battery voltage: %.2f V", (double)*voltage);

#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	return 0;
}

static int init(void)
{
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup(m_dev, &m_channel_cfg);
	if (ret) {
		LOG_ERR("Call `adc_channel_setup` failed: %d", ret);
		return ret;
	}

#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	return 0;
}

SYS_INIT(init, APPLICATION, 0);
