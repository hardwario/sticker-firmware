/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator ADC: every read returns the pin voltage set for the
 * channel (sim_adc_set_mv()), converted to a raw sample against the internal
 * 3300 mV reference. It has a PM action, as the STM32 ADC the application
 * suspends and resumes around each read. */

#define DT_DRV_COMPAT hardwario_sim_adc

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <errno.h>

#define SIM_ADC_CHANNELS 16
#define SIM_ADC_REF_MV   3300

/* Defaults: a 3.0 V battery behind the 560k/100k divider, inputs at 0 V. */
static int32_t m_mv[SIM_ADC_CHANNELS] = {[2] = 3000 * 100 / 660};
static struct k_spinlock m_lock;

void sim_adc_set_mv(uint8_t channel, int32_t mv)
{
	if (channel < SIM_ADC_CHANNELS) {
		K_SPINLOCK(&m_lock) {
			m_mv[channel] = CLAMP(mv, 0, SIM_ADC_REF_MV);
		}
	}
}

int32_t sim_adc_get_mv(uint8_t channel)
{
	int32_t mv = 0;

	if (channel < SIM_ADC_CHANNELS) {
		K_SPINLOCK(&m_lock) {
			mv = m_mv[channel];
		}
	}

	return mv;
}

static int sim_adc_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg)
{
	ARG_UNUSED(dev);

	if (cfg->channel_id >= SIM_ADC_CHANNELS || cfg->gain != ADC_GAIN_1 ||
	    cfg->reference != ADC_REF_INTERNAL) {
		return -ENOTSUP;
	}

	return 0;
}

static int sim_adc_read(const struct device *dev, const struct adc_sequence *seq)
{
	ARG_UNUSED(dev);

	if (seq->resolution == 0 || seq->resolution > 16) {
		return -EINVAL;
	}

	uint16_t *out = seq->buffer;
	size_t n = 0;

	for (uint8_t ch = 0; ch < SIM_ADC_CHANNELS; ch++) {
		if (!(seq->channels & BIT(ch))) {
			continue;
		}
		if ((n + 1) * sizeof(uint16_t) > seq->buffer_size) {
			return -ENOMEM;
		}
		out[n++] = (uint16_t)(sim_adc_get_mv(ch) * (BIT(seq->resolution) - 1) /
				      SIM_ADC_REF_MV);
	}

	return 0;
}

static int sim_adc_pm_action(const struct device *dev, enum pm_device_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	return 0;
}

static int sim_adc_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static DEVICE_API(adc, sim_adc_api) = {
	.channel_setup = sim_adc_channel_setup,
	.read = sim_adc_read,
	.ref_internal = SIM_ADC_REF_MV,
};

PM_DEVICE_DT_INST_DEFINE(0, sim_adc_pm_action);

DEVICE_DT_INST_DEFINE(0, sim_adc_init, PM_DEVICE_DT_INST_GET(0), NULL, NULL, POST_KERNEL,
		      CONFIG_ADC_INIT_PRIORITY, &sim_adc_api);
