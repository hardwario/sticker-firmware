/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_analog.h"
#include "app_config.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_analog, LOG_LEVEL_DBG);

/* GP_A = PB4 = ADC1_IN3, GP_B = PA11 = ADC1_IN7. */
#define ADC_CHANNEL_A 3
#define ADC_CHANNEL_B 7

/* External divider on the GP_A/GP_B front-end (JP11): series 33k (R2/R3),
 * shunt 1k (R5/R7) to GND. It scales the 0..24 V input into the pin's 0..VDD
 * range; the reported value is input-referred, V_in = V_pin * 34. Both
 * channels are populated identically (R8/R9 DNP). The ~6 mV pin offset of the
 * 560k bias to VDD (R4/R6) is left uncompensated, like the battery path. */
#define R_SERIES_KOHM 33
#define R_SHUNT_KOHM  1

static const struct device *m_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

static const struct gpio_dt_spec m_pin_a = GPIO_DT_SPEC_GET(DT_NODELABEL(gp_a), gpios);
static const struct gpio_dt_spec m_pin_b = GPIO_DT_SPEC_GET(DT_NODELABEL(gp_b), gpios);

/* Serializes the whole RESUME -> adc_read -> SUSPEND critical section below.
 * pm_device_action_run() is the raw, non-refcounted PM API (deliberately, to
 * avoid pulling in full PM-device runtime bookkeeping for a single ADC) --
 * it does NOT queue or block a concurrent caller against another in-flight
 * RESUME/SUSPEND pair. Two callers interleaving (e.g. app_sensor_init()'s
 * boot-time sample vs. a P2P instant ready-kick's inline sample, #118) can
 * have one thread's SUSPEND land between the other's RESUME and its
 * set_sequencer(), powering the ADC down right as the second thread starts
 * a conversion -- the STM32 ADC driver's CCRDY wait is an unbounded busy-spin
 * on a powered-down ADC, hanging that thread (and whatever work queue it
 * runs on) forever. Confirmed via a live GDB backtrace during #118 HIL
 * testing. One lock for every ADC1 user (battery + GP_A/GP_B, #396). */
static K_MUTEX_DEFINE(m_lock);

int app_analog_read_mv(uint8_t channel_id, int32_t *mv)
{
	int ret;

	if (!device_is_ready(m_dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_RESUME);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		k_mutex_unlock(&m_lock);
		return ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	int res = 0;
	int16_t sample;

	/* Set the channel up on every read, while ADC1 is clocked: a setup done
	 * while it is suspended (clock gated) would not reach the registers. */
	const struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_MAX,
		.channel_id = channel_id,
		.differential = 0,
	};

	struct adc_sequence seq = {
		.channels = BIT(channel_id),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	ret = adc_channel_setup(m_dev, &cfg);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_channel_setup", ret);
		res = ret;
		goto suspend;
	}

	ret = adc_read(m_dev, &seq);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_read", ret);
		res = ret;
		goto suspend;
	}

	int32_t v = sample;
	ret = adc_raw_to_millivolts(adc_ref_internal(m_dev), cfg.gain, seq.resolution, &v);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("adc_raw_to_millivolts", ret);
		res = ret;
		goto suspend;
	}

	LOG_DBG("Channel %u: %d mV (raw: %d)", channel_id, v, sample);

	*mv = v;

suspend:
#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		res = res ? res : ret;
	}
#endif /* defined(CONFIG_PM_DEVICE) */

	k_mutex_unlock(&m_lock);
	return res;
}

static int set_analog(const struct gpio_dt_spec *spec)
{
	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}

	/* No direction flag = analog mode on STM32 (digital input buffer off). Done
	 * here rather than through the ADC's pinctrl: the STM32 ADC driver
	 * re-applies its default pinctrl on every PM resume, which would pull the
	 * pin away from the digital input / PIR / buzzer sharing it. */
	int ret = gpio_pin_configure_dt(spec, GPIO_DISCONNECTED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	return 0;
}

int app_analog_init(void)
{
	int ret;

	if (g_app_config.cap_analog_a) {
		ret = set_analog(&m_pin_a);
		if (ret) {
			return ret;
		}
	}

	if (g_app_config.cap_analog_b) {
		ret = set_analog(&m_pin_b);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

static int measure_input(uint8_t channel_id, float *voltage)
{
	int32_t pin_mv;

	int ret = app_analog_read_mv(channel_id, &pin_mv);
	if (ret) {
		return ret;
	}

	/* Divider-correct to the input-referred voltage at the connector. */
	int32_t input_mv = pin_mv * (R_SERIES_KOHM + R_SHUNT_KOHM) / R_SHUNT_KOHM;

	LOG_DBG("Channel %u: input %d mV", channel_id, input_mv);

	*voltage = input_mv / 1000.f;

	return 0;
}

int app_analog_measure_a(float *voltage)
{
	return measure_input(ADC_CHANNEL_A, voltage);
}

int app_analog_measure_b(float *voltage)
{
	return measure_input(ADC_CHANNEL_B, voltage);
}
