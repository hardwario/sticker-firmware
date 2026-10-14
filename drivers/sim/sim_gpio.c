/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: an input-only GPIO port for interrupt lines that an
 * emulator drives (sim_gpio_input_set). Edge interrupts fire on a level
 * change, level interrupts while the line is active (also when enabled on an
 * active line). Configuring a pin runs no callbacks (cf. gpio_emul). */

#define DT_DRV_COMPAT hardwario_sim_gpio

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <errno.h>

struct sim_gpio_config {
	struct gpio_driver_config common;
};

struct sim_gpio_data {
	struct gpio_driver_data common;
	sys_slist_t callbacks;
	struct k_spinlock lock;
	gpio_port_value_t levels;
	gpio_port_pins_t enabled, edge, rising, falling; /* level: rising = high */
};

static int sim_gpio_pin_configure(const struct device *port, gpio_pin_t pin, gpio_flags_t flags)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pin);

	return (flags & GPIO_OUTPUT) ? -ENOTSUP : 0;
}

static int sim_gpio_port_get_raw(const struct device *port, gpio_port_value_t *value)
{
	struct sim_gpio_data *data = port->data;

	*value = data->levels;

	return 0;
}

static int sim_gpio_port_set_masked_raw(const struct device *port, gpio_port_pins_t mask,
					gpio_port_value_t value)
{
	ARG_UNUSED(port);
	ARG_UNUSED(mask);
	ARG_UNUSED(value);

	return -ENOTSUP;
}

static int sim_gpio_port_set_bits_raw(const struct device *port, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	return -ENOTSUP;
}

static int sim_gpio_port_toggle_bits(const struct device *port, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	return -ENOTSUP;
}

/* Level interrupts that are due now. */
static gpio_port_pins_t level_due(struct sim_gpio_data *data)
{
	gpio_port_pins_t level = data->enabled & ~data->edge;

	return level & ((data->levels & data->rising) | (~data->levels & data->falling));
}

static int sim_gpio_pin_interrupt_configure(const struct device *port, gpio_pin_t pin,
					    enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	struct sim_gpio_data *data = port->data;
	gpio_port_pins_t bit = BIT(pin);
	gpio_port_pins_t due;

	K_SPINLOCK(&data->lock) {
		data->enabled &= ~bit;
		data->edge &= ~bit;
		data->rising &= ~bit;
		data->falling &= ~bit;
		if (mode != GPIO_INT_MODE_DISABLED) {
			data->enabled |= bit;
			data->edge |= (mode == GPIO_INT_MODE_EDGE) ? bit : 0;
			data->rising |= (trig & GPIO_INT_TRIG_HIGH) ? bit : 0;
			data->falling |= (trig & GPIO_INT_TRIG_LOW) ? bit : 0;
		}
		due = level_due(data) & bit;
	}
	if (due) {
		gpio_fire_callbacks(&data->callbacks, port, due);
	}

	return 0;
}

static int sim_gpio_manage_callback(const struct device *port, struct gpio_callback *cb, bool set)
{
	struct sim_gpio_data *data = port->data;

	return gpio_manage_callback(&data->callbacks, cb, set);
}

int sim_gpio_input_set(const struct device *port, gpio_pin_t pin, int level)
{
	struct sim_gpio_data *data = port->data;
	gpio_port_pins_t bit = BIT(pin);
	gpio_port_pins_t fire = 0;

	K_SPINLOCK(&data->lock) {
		bool old = data->levels & bit;

		data->levels = level ? (data->levels | bit) : (data->levels & ~bit);
		if (old != !!level && (data->enabled & data->edge & bit)) {
			fire = (level ? data->rising : data->falling) & bit;
		}
		fire |= level_due(data) & bit;
	}
	if (fire) {
		gpio_fire_callbacks(&data->callbacks, port, fire);
	}

	return 0;
}

static DEVICE_API(gpio, sim_gpio_api) = {
	.pin_configure = sim_gpio_pin_configure,
	.port_get_raw = sim_gpio_port_get_raw,
	.port_set_masked_raw = sim_gpio_port_set_masked_raw,
	.port_set_bits_raw = sim_gpio_port_set_bits_raw,
	.port_clear_bits_raw = sim_gpio_port_set_bits_raw,
	.port_toggle_bits = sim_gpio_port_toggle_bits,
	.pin_interrupt_configure = sim_gpio_pin_interrupt_configure,
	.manage_callback = sim_gpio_manage_callback,
};

#define SIM_GPIO_DEFINE(n)                                                                         \
	static const struct sim_gpio_config sim_gpio_config_##n = {                                \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n)},                   \
	};                                                                                         \
	static struct sim_gpio_data sim_gpio_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &sim_gpio_data_##n, &sim_gpio_config_##n,             \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &sim_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(SIM_GPIO_DEFINE)
