/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: Maxim DS2484 I2C-to-1-Wire bridge with DS18B20
 * thermometers on its bus.
 *
 * Bridge: the commands of the Zephyr driver (w1_ds2482_84_common.h) - device
 * reset, set read pointer, write config, adjust port, 1-Wire reset, single
 * bit, write/read byte and triplet - complete at once (1WB never set).
 *
 * Bus: time slots. In each slot every selected device either drives its next
 * bit (wired-AND with the master's bit) or samples the bus, so search ROM,
 * read ROM, match/skip ROM and the DS18B20 function commands (convert T,
 * read/write scratchpad, copy, recall, read power supply) work bit by bit as
 * on the wire. A conversion finishes immediately. DS28E17 machine probes are
 * not emulated. */

#define DT_DRV_COMPAT maxim_ds2484

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <math.h>
#include <string.h>

/* Bridge commands and registers. */
#define CMD_1WT  0x78
#define CMD_1WSB 0x87
#define CMD_1WRB 0x96
#define CMD_1WWB 0xa5
#define CMD_1WRS 0xb4
#define CMD_ADJP 0xc3
#define CMD_WCFG 0xd2
#define CMD_SRP  0xe1
#define CMD_DRST 0xf0

#define REG_CONFIG 0xc3
#define REG_DATA   0xe1
#define REG_STATUS 0xf0
#define REG_PORT   0xb4

#define STATUS_PPD BIT(1)
#define STATUS_LL  BIT(3)
#define STATUS_RST BIT(4)
#define STATUS_SBR BIT(5)
#define STATUS_TSB BIT(6)
#define STATUS_DIR BIT(7)

/* 1-Wire ROM and DS18B20 function commands. */
#define ROM_SEARCH       0xf0
#define ROM_READ         0x33
#define ROM_MATCH        0x55
#define ROM_SKIP         0xcc
#define ROM_ALARM_SEARCH 0xec
#define FN_CONVERT_T     0x44
#define FN_WRITE_SP      0x4e
#define FN_READ_SP       0xbe
#define FN_COPY_SP       0x48
#define FN_RECALL        0xb8
#define FN_READ_POWER    0xb4

#define FAMILY_DS18B20 0x28

enum dev_state {
	ST_IDLE,    /* deselected until the next reset */
	ST_ROM_CMD, /* receiving the ROM command */
	ST_SEARCH,  /* search ROM: bit, complement, direction per ROM bit */
	ST_MATCH,   /* receiving the 64-bit ROM */
	ST_FN_CMD,  /* receiving the function command */
	ST_RX,      /* receiving function data (write scratchpad) */
	ST_TX,      /* sending bits */
};

struct w1_dev {
	bool present;
	float temperature;
	uint8_t rom[8];
	uint8_t sp[9]; /* scratchpad, sp[8] = CRC */
	enum dev_state state;
	uint8_t rx[8];
	int rx_bits, rx_len;
	uint8_t tx[9];
	int tx_bit, tx_len; /* bits; tx_len < 0: endless 1s (done / powered) */
	int search_bit, search_phase;
};

static struct w1_dev m_dev[SIM_W1_SENSORS];
static bool m_bridge_present = true;
static uint8_t m_ptr = REG_STATUS;
static uint8_t m_status = STATUS_LL | STATUS_RST;
static uint8_t m_config;
static uint8_t m_data;
static uint8_t m_port;
static uint32_t m_resets;
static struct k_spinlock m_lock;

static uint8_t w1_crc(const uint8_t *buf, size_t len)
{
	return crc8(buf, len, 0x8c, 0x00, true); /* reflected 0x31 */
}

static void update_scratchpad(struct w1_dev *d)
{
	int16_t raw = (int16_t)lroundf(CLAMP(d->temperature, -55.0f, 125.0f) * 16.0f);
	int res = 9 + ((d->sp[4] >> 5) & 0x3);

	raw &= ~((1 << (12 - res)) - 1); /* undefined LSBs read as 0 */
	d->sp[0] = (uint8_t)raw;
	d->sp[1] = (uint8_t)((uint16_t)raw >> 8);
	d->sp[5] = 0xff;
	d->sp[6] = 0x0c;
	d->sp[7] = 0x10;
	d->sp[8] = w1_crc(d->sp, 8);
}

static void dev_init(int i)
{
	struct w1_dev *d = &m_dev[i];

	d->rom[0] = FAMILY_DS18B20;
	/* Serial "STK" + index, readable in a ROM dump. */
	d->rom[1] = 0x01 + i;
	d->rom[2] = 0x00;
	d->rom[3] = 'K';
	d->rom[4] = 'T';
	d->rom[5] = 'S';
	d->rom[6] = 0x00;
	d->rom[7] = w1_crc(d->rom, 7);
	d->sp[2] = 0x4b; /* TH, TL, config: power-on defaults, 12 bit */
	d->sp[3] = 0x46;
	d->sp[4] = 0x7f;
	d->sp[0] = 0x50; /* +85 °C power-on reading */
	d->sp[1] = 0x05;
	d->sp[5] = 0xff;
	d->sp[6] = 0x0c;
	d->sp[7] = 0x10;
	d->sp[8] = w1_crc(d->sp, 8);
}

static void dev_send(struct w1_dev *d, const uint8_t *buf, int len)
{
	memcpy(d->tx, buf, len);
	d->tx_bit = 0;
	d->tx_len = len * 8;
	d->state = ST_TX;
}

static void dev_send_ones(struct w1_dev *d)
{
	d->tx_bit = 0;
	d->tx_len = -1;
	d->state = ST_TX;
}

static void dev_rx_start(struct w1_dev *d, enum dev_state state, int len)
{
	d->state = state;
	d->rx_bits = 0;
	d->rx_len = len;
	memset(d->rx, 0, sizeof(d->rx));
}

/* One whole byte received in a receiving state. */
static void dev_rx_done(struct w1_dev *d)
{
	switch (d->state) {
	case ST_ROM_CMD:
		switch (d->rx[0]) {
		case ROM_READ:
			dev_send(d, d->rom, 8);
			break;
		case ROM_SKIP:
			dev_rx_start(d, ST_FN_CMD, 1);
			break;
		case ROM_MATCH:
			dev_rx_start(d, ST_MATCH, 8);
			break;
		case ROM_SEARCH:
			d->state = ST_SEARCH;
			d->search_bit = 0;
			d->search_phase = 0;
			break;
		default: /* alarm search (no alarms) and the rest */
			d->state = ST_IDLE;
			break;
		}
		break;
	case ST_MATCH:
		if (memcmp(d->rx, d->rom, 8) == 0) {
			dev_rx_start(d, ST_FN_CMD, 1);
		} else {
			d->state = ST_IDLE;
		}
		break;
	case ST_FN_CMD:
		switch (d->rx[0]) {
		case FN_CONVERT_T:
			update_scratchpad(d);
			dev_send_ones(d); /* conversion done */
			break;
		case FN_READ_SP:
			dev_send(d, d->sp, 9);
			break;
		case FN_WRITE_SP:
			dev_rx_start(d, ST_RX, 3);
			break;
		case FN_RECALL:
		case FN_READ_POWER: /* externally powered */
		case FN_COPY_SP:
			dev_send_ones(d);
			break;
		default:
			d->state = ST_IDLE;
			break;
		}
		break;
	case ST_RX:
		memcpy(&d->sp[2], d->rx, 3);
		d->sp[8] = w1_crc(d->sp, 8);
		d->state = ST_IDLE;
		break;
	default:
		break;
	}
}

/* The bit a device drives in this slot: 0 or 1, or -1 when it only samples. */
static int dev_drive(struct w1_dev *d)
{
	if (d->state == ST_TX) {
		if (d->tx_len < 0) {
			return 1;
		}
		return d->tx_bit < d->tx_len ? (d->tx[d->tx_bit / 8] >> (d->tx_bit % 8)) & 1 : 1;
	}
	if (d->state == ST_SEARCH && d->search_phase < 2) {
		int bit = (d->rom[d->search_bit / 8] >> (d->search_bit % 8)) & 1;

		return d->search_phase == 0 ? bit : !bit;
	}

	return -1;
}

/* End of the slot: the device sees the bus level. */
static void dev_sample(struct w1_dev *d, int bus)
{
	switch (d->state) {
	case ST_TX:
		if (d->tx_len >= 0 && ++d->tx_bit >= d->tx_len) {
			d->state = ST_IDLE;
		}
		break;
	case ST_SEARCH:
		if (d->search_phase < 2) {
			d->search_phase++;
			break;
		}
		if (bus != ((d->rom[d->search_bit / 8] >> (d->search_bit % 8)) & 1)) {
			d->state = ST_IDLE; /* the master went the other way */
			break;
		}
		d->search_phase = 0;
		if (++d->search_bit == 64) {
			dev_rx_start(d, ST_FN_CMD, 1);
		}
		break;
	case ST_ROM_CMD:
	case ST_MATCH:
	case ST_FN_CMD:
	case ST_RX:
		d->rx[d->rx_bits / 8] |= bus << (d->rx_bits % 8);
		if (++d->rx_bits == d->rx_len * 8) {
			dev_rx_done(d);
		}
		break;
	default:
		break;
	}
}

static bool bus_reset(void)
{
	bool presence = false;

	m_resets++;
	for (int i = 0; i < SIM_W1_SENSORS; i++) {
		if (m_dev[i].present) {
			dev_rx_start(&m_dev[i], ST_ROM_CMD, 1);
			presence = true;
		} else {
			m_dev[i].state = ST_IDLE;
		}
	}

	return presence;
}

static int bus_slot(int master)
{
	int bus = master;

	for (int i = 0; i < SIM_W1_SENSORS; i++) {
		if (m_dev[i].present) {
			int bit = dev_drive(&m_dev[i]);

			if (bit == 0) {
				bus = 0;
			}
		}
	}
	for (int i = 0; i < SIM_W1_SENSORS; i++) {
		if (m_dev[i].present) {
			dev_sample(&m_dev[i], bus);
		}
	}

	return bus;
}

static uint8_t bus_byte(uint8_t out)
{
	uint8_t in = 0;

	for (int b = 0; b < 8; b++) {
		in |= bus_slot((out >> b) & 1) << b;
	}

	return in;
}

static void bridge_command(uint8_t cmd, const uint8_t *arg, int arg_len)
{
	m_ptr = REG_STATUS;

	switch (cmd) {
	case CMD_DRST:
		m_config = 0;
		m_status = STATUS_LL | STATUS_RST;
		break;
	case CMD_SRP:
		if (arg_len) {
			m_ptr = arg[0];
		}
		break;
	case CMD_WCFG:
		/* Upper nibble must be the complement of the lower one. */
		if (arg_len && ((arg[0] >> 4) ^ (arg[0] & 0x0f)) == 0x0f) {
			m_config = arg[0] & 0x0f;
			m_status &= ~STATUS_RST;
		}
		m_ptr = REG_CONFIG;
		break;
	case CMD_ADJP:
		if (arg_len) {
			m_port = arg[0];
		}
		m_ptr = REG_PORT;
		break;
	case CMD_1WRS:
		m_status = (m_status & ~(STATUS_PPD | STATUS_RST)) | (bus_reset() ? STATUS_PPD : 0);
		break;
	case CMD_1WSB: {
		int bit = bus_slot(arg_len && (arg[0] & BIT(7)));

		m_status = (m_status & ~STATUS_SBR) | (bit ? STATUS_SBR : 0);
		break;
	}
	case CMD_1WWB:
		if (arg_len) {
			(void)bus_byte(arg[0]);
		}
		break;
	case CMD_1WRB:
		m_data = bus_byte(0xff);
		break;
	case CMD_1WT: {
		int id = bus_slot(1);
		int cmp = bus_slot(1);
		int dir = (id != cmp) ? id : (arg_len && (arg[0] & BIT(7)));

		(void)bus_slot(dir);
		m_status = (m_status & ~(STATUS_SBR | STATUS_TSB | STATUS_DIR)) |
			   (id ? STATUS_SBR : 0) | (cmp ? STATUS_TSB : 0) | (dir ? STATUS_DIR : 0);
		break;
	}
	default:
		break;
	}
}

static uint8_t bridge_read(void)
{
	switch (m_ptr) {
	case REG_DATA:
		return m_data;
	case REG_CONFIG:
		return m_config;
	case REG_PORT:
		return m_port;
	default:
		return m_status;
	}
}

void sim_w1_set(int index, float temperature)
{
	if (index < 0 || index >= SIM_W1_SENSORS) {
		return;
	}
	K_SPINLOCK(&m_lock) {
		m_dev[index].temperature = temperature;
	}
}

void sim_w1_set_present(int index, bool present)
{
	K_SPINLOCK(&m_lock) {
		if (index < 0) {
			m_bridge_present = present;
		} else if (index < SIM_W1_SENSORS) {
			m_dev[index].present = present;
			m_dev[index].state = ST_IDLE;
		}
	}
}

void sim_w1_get(int index, float *temperature, bool *present, uint8_t rom[8])
{
	if (index < 0 || index >= SIM_W1_SENSORS) {
		return;
	}
	K_SPINLOCK(&m_lock) {
		*temperature = m_dev[index].temperature;
		*present = m_dev[index].present;
		memcpy(rom, m_dev[index].rom, 8);
	}
}

void sim_w1_bus(bool *present, uint32_t *resets)
{
	K_SPINLOCK(&m_lock) {
		*present = m_bridge_present;
		*resets = m_resets;
	}
}

static int emul_ds2484_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);
	int ret = 0;

	K_SPINLOCK(&m_lock) {
		if (!m_bridge_present) {
			ret = -EIO;
			K_SPINLOCK_BREAK;
		}
		for (int i = 0; i < num_msgs; i++) {
			struct i2c_msg *msg = &msgs[i];

			if (msg->flags & I2C_MSG_READ) {
				for (uint32_t j = 0; j < msg->len; j++) {
					msg->buf[j] = bridge_read();
				}
			} else if (msg->len > 0) {
				bridge_command(msg->buf[0], &msg->buf[1], msg->len - 1);
			}
		}
	}

	return ret;
}

static const struct i2c_emul_api emul_ds2484_api = {
	.transfer = emul_ds2484_transfer,
};

static int emul_ds2484_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	for (int i = 0; i < SIM_W1_SENSORS; i++) {
		dev_init(i);
		m_dev[i].temperature = 20.0f + i;
		m_dev[i].present = i < 2;
	}

	return 0;
}

EMUL_DT_INST_DEFINE(0, emul_ds2484_init, NULL, NULL, &emul_ds2484_api, NULL);
