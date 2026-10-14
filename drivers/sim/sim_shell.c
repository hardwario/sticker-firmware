/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: the `sim` shell command sets what the emulated
 * peripherals report. Values are physical (°C, %, lux, V); pins are driven at
 * their physical level, so an active-low hall sensor sees a magnet at 0. */

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <stdlib.h>
#include <string.h>

/* Board dividers, as in app_battery.c / app_analog.c. */
#define BATT_DIV_NUM  100 /* R2 / (R1 + R2), 560k / 100k */
#define BATT_DIV_DEN  660
#define INPUT_DIV_DEN 34 /* 33k series, 1k shunt */
#define ADC_CH_BATT   2
#define ADC_CH_A      3
#define ADC_CH_B      7

struct sim_pin {
	const char *name;
	struct gpio_dt_spec spec;
};

static const struct sim_pin m_pins[] = {
	{"hall_l", GPIO_DT_SPEC_GET(DT_NODELABEL(hall_l), gpios)},
	{"hall_r", GPIO_DT_SPEC_GET(DT_NODELABEL(hall_r), gpios)},
	{"gp_a", GPIO_DT_SPEC_GET(DT_NODELABEL(gp_a), gpios)},
	{"gp_b", GPIO_DT_SPEC_GET(DT_NODELABEL(gp_b), gpios)},
	{"nfc_gpo", GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), nfc_gpo_gpios)},
};

/* Level each pin is driven to from outside (gpio_emul has no input getter).
 * Idle: hall sensors without a magnet and the GP inputs high, NFC GPO low.
 * The external source wins over the MCU pull: gpio_emul sets the input from
 * the pull flags on every configure, so a callback re-applies this level
 * (app_hall.c switches the pull between samples). */
static int m_pin_level[ARRAY_SIZE(m_pins)] = {1, 1, 1, 1, 0};
static struct gpio_callback m_pin_cb[ARRAY_SIZE(m_pins)];

static const struct sim_pin *find_pin(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_pins); i++) {
		if (strcmp(m_pins[i].name, name) == 0) {
			return &m_pins[i];
		}
	}

	return NULL;
}

static int pin_set(const struct sim_pin *pin, int level)
{
	gpio_flags_t flags = 0;

	m_pin_level[pin - m_pins] = level;

	/* A pin the application has not configured as an input (its feature
	 * is off) has nothing to drive yet; gpio_emul would log an error. */
	(void)gpio_emul_flags_get(pin->spec.port, pin->spec.pin, &flags);
	if (!(flags & GPIO_INPUT)) {
		return 0;
	}

	return gpio_emul_input_set(pin->spec.port, pin->spec.pin, level);
}

static void pin_cb_handler(const struct device *port, struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	const struct sim_pin *pin = &m_pins[cb - m_pin_cb];
	gpio_flags_t flags = 0;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	(void)gpio_emul_flags_get(pin->spec.port, pin->spec.pin, &flags);
	if (flags & GPIO_INPUT) {
		/* No-op when the level already matches, so no recursion. */
		(void)gpio_emul_input_set(pin->spec.port, pin->spec.pin, m_pin_level[pin - m_pins]);
	}
}

static int pin_get(const struct sim_pin *pin)
{
	return m_pin_level[pin - m_pins];
}

/* Pulse train on one pin, run from its own work item so the shell returns:
 * `count` pulses of `width` ms away from the idle level, `width` ms apart,
 * each edge preceded by `bounce` extra toggles 1 ms apart (contact bounce). */
static struct {
	struct k_work_delayable work;
	const struct sim_pin *pin;
	int idle;
	uint32_t count;
	uint32_t width_ms;
	uint32_t bounce;
	uint32_t step;
	uint32_t bounce_left;
} m_pulse;

static void pulse_work(struct k_work *work)
{
	ARG_UNUSED(work);

	if (m_pulse.count == 0) {
		(void)pin_set(m_pulse.pin, m_pulse.idle);
		return;
	}

	/* step even = go active, odd = go idle */
	int target = (m_pulse.step & 1) ? m_pulse.idle : !m_pulse.idle;

	if (m_pulse.bounce_left > 0) {
		/* Bounce: toggle around the target, 1 ms per toggle. */
		(void)pin_set(m_pulse.pin, (m_pulse.bounce_left & 1) ? target : !target);
		m_pulse.bounce_left--;
		k_work_schedule(&m_pulse.work, K_MSEC(1));
		return;
	}

	(void)pin_set(m_pulse.pin, target);
	m_pulse.step++;
	m_pulse.bounce_left = m_pulse.bounce * 2;
	if (!(m_pulse.step & 1)) {
		m_pulse.count--;
	}
	k_work_schedule(&m_pulse.work, K_MSEC(m_pulse.width_ms));
}

static int parse_float(const struct shell *sh, const char *s, float *out)
{
	char *end;

	*out = strtof(s, &end);
	if (end == s || *end != '\0') {
		shell_error(sh, "not a number: %s", s);
		return -EINVAL;
	}

	return 0;
}

static int cmd_sht(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0)) {
		sim_sht4x_set_present(strcmp(argv[1], "on") == 0);
		return 0;
	}
	if (argc != 3) {
		shell_error(sh, "usage: sim sht <temperature °C> <humidity %%> | on | off");
		return -EINVAL;
	}

	float t, rh;

	if (parse_float(sh, argv[1], &t) || parse_float(sh, argv[2], &rh)) {
		return -EINVAL;
	}
	sim_sht4x_set(t, rh);
	sim_sht4x_set_present(true);

	return 0;
}

static int cmd_light(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0) {
		sim_opt3001_set_present(strcmp(argv[1], "on") == 0);
		return 0;
	}

	float lux;

	if (parse_float(sh, argv[1], &lux)) {
		return -EINVAL;
	}
	sim_opt3001_set(lux);
	sim_opt3001_set_present(true);

	return 0;
}

static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	float v;

	if (parse_float(sh, argv[1], &v)) {
		return -EINVAL;
	}
	sim_adc_set_mv(ADC_CH_BATT, (int32_t)(v * 1000.0f * BATT_DIV_NUM / BATT_DIV_DEN));

	return 0;
}

static int cmd_analog(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	float v;
	uint8_t ch;

	if (strcmp(argv[1], "a") == 0) {
		ch = ADC_CH_A;
	} else if (strcmp(argv[1], "b") == 0) {
		ch = ADC_CH_B;
	} else {
		shell_error(sh, "input must be a or b");
		return -EINVAL;
	}
	if (parse_float(sh, argv[2], &v)) {
		return -EINVAL;
	}
	sim_adc_set_mv(ch, (int32_t)(v * 1000.0f / INPUT_DIV_DEN));

	return 0;
}

static int cmd_pin(const struct shell *sh, size_t argc, char **argv)
{
	const struct sim_pin *pin = find_pin(argv[1]);

	if (pin == NULL) {
		shell_error(sh, "unknown pin %s", argv[1]);
		return -EINVAL;
	}
	if (argc == 2) {
		shell_print(sh, "%s = %d", pin->name, pin_get(pin));
		return 0;
	}

	return pin_set(pin, atoi(argv[2]) ? 1 : 0);
}

static int cmd_pulse(const struct shell *sh, size_t argc, char **argv)
{
	const struct sim_pin *pin = find_pin(argv[1]);

	if (pin == NULL) {
		shell_error(sh, "unknown pin %s", argv[1]);
		return -EINVAL;
	}
	if (k_work_delayable_is_pending(&m_pulse.work)) {
		shell_error(sh, "a pulse train is running");
		return -EBUSY;
	}

	m_pulse.pin = pin;
	m_pulse.idle = pin_get(pin);
	m_pulse.count = strtoul(argv[2], NULL, 0);
	m_pulse.width_ms = argc > 3 ? MAX(strtoul(argv[3], NULL, 0), 1u) : 50;
	m_pulse.bounce = argc > 4 ? strtoul(argv[4], NULL, 0) : 0;
	m_pulse.step = 0;
	m_pulse.bounce_left = m_pulse.bounce * 2;
	k_work_schedule(&m_pulse.work, K_NO_WAIT);

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	float t, rh, lux;
	bool sht, opt;

	sim_sht4x_get(&t, &rh, &sht);
	sim_opt3001_get(&lux, &opt);

	shell_print(sh, "sht      %s %.2f °C %.2f %%", sht ? "on" : "off", (double)t, (double)rh);
	shell_print(sh, "light    %s %.2f lux", opt ? "on" : "off", (double)lux);
	shell_print(sh, "battery  %.3f V",
		    (double)(sim_adc_get_mv(ADC_CH_BATT) * BATT_DIV_DEN / BATT_DIV_NUM / 1000.0f));
	shell_print(sh, "analog a %.3f V, b %.3f V",
		    (double)(sim_adc_get_mv(ADC_CH_A) * INPUT_DIV_DEN / 1000.0f),
		    (double)(sim_adc_get_mv(ADC_CH_B) * INPUT_DIV_DEN / 1000.0f));
	for (size_t i = 0; i < ARRAY_SIZE(m_pins); i++) {
		shell_print(sh, "pin %-8s %d", m_pins[i].name, pin_get(&m_pins[i]));
	}

	return 0;
}

static int sim_shell_init(void)
{
	k_work_init_delayable(&m_pulse.work, pulse_work);

	for (size_t i = 0; i < ARRAY_SIZE(m_pins); i++) {
		gpio_init_callback(&m_pin_cb[i], pin_cb_handler, BIT(m_pins[i].spec.pin));
		(void)gpio_add_callback(m_pins[i].spec.port, &m_pin_cb[i]);
	}

	return 0;
}

SYS_INIT(sim_shell_init, POST_KERNEL, 99);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_sim, SHELL_CMD_ARG(status, NULL, "Show simulated readings.", cmd_status, 1, 0),
	SHELL_CMD_ARG(sht, NULL, "SHT4x: sht <°C> <%RH> | on | off", cmd_sht, 2, 1),
	SHELL_CMD_ARG(light, NULL, "OPT3001: light <lux> | on | off", cmd_light, 2, 0),
	SHELL_CMD_ARG(battery, NULL, "Battery voltage: battery <V>", cmd_battery, 2, 0),
	SHELL_CMD_ARG(analog, NULL, "GP input voltage: analog a|b <V>", cmd_analog, 3, 0),
	SHELL_CMD_ARG(pin, NULL, "Physical pin level: pin hall_l|hall_r|gp_a|gp_b|nfc_gpo [0|1]",
		      cmd_pin, 2, 1),
	SHELL_CMD_ARG(pulse, NULL,
		      "Pulse train: pulse <pin> <count> [width ms, default 50] [bounce toggles]",
		      cmd_pulse, 3, 2),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sim, &sub_sim, "STICKER simulator peripherals.", NULL);
