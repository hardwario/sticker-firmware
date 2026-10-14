/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: Semtech SX1262 on the emulated SPI bus, under the real
 * Zephyr sx126x driver, LoRaMac radio.c and LoRaMac. It decodes the SPI
 * command set, keeps the register file and data buffer, times TX/RX by the
 * modem's time on air and raises the IRQs on DIO1 (gpio_emul). BUSY never
 * goes high.
 *
 * Packets go to the "air" peer on the host (sim_air.h). A received packet is
 * queued with the time its transmission starts; a receive window catches it
 * when the modulation matches and the window is open for its preamble, so
 * LoRaWAN RX1/RX2 timing is decided here in simulated time and does not
 * depend on the host's real time. After a transmission, the first receive
 * blocks (host time) until the peer has answered that packet, when a peer is
 * present. */

#define DT_DRV_COMPAT semtech_sx1262

#include "sim.h"
#include "sim_air.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include "cmdline.h"
#include "soc.h"

LOG_MODULE_REGISTER(emul_sx126x, LOG_LEVEL_INF);

/* Opcodes (SX1261/2 datasheet, table 11-1) */
#define OP_GET_STATUS        0xc0
#define OP_WRITE_REGISTER    0x0d
#define OP_READ_REGISTER     0x1d
#define OP_WRITE_BUFFER      0x0e
#define OP_READ_BUFFER       0x1e
#define OP_SET_SLEEP         0x84
#define OP_SET_STANDBY       0x80
#define OP_SET_FS            0xc1
#define OP_SET_TX            0x83
#define OP_SET_RX            0x82
#define OP_SET_CAD           0xc5
#define OP_SET_TX_CW         0xd1
#define OP_SET_TX_PREAMBLE   0xd2
#define OP_SET_PACKET_TYPE   0x8a
#define OP_GET_PACKET_TYPE   0x11
#define OP_SET_RF_FREQUENCY  0x86
#define OP_SET_TX_PARAMS     0x8e
#define OP_SET_BUFFER_BASE   0x8f
#define OP_SET_MOD_PARAMS    0x8b
#define OP_SET_PKT_PARAMS    0x8c
#define OP_GET_RX_BUF_STATUS 0x13
#define OP_GET_PKT_STATUS    0x14
#define OP_GET_RSSI_INST     0x15
#define OP_GET_STATS         0x10
#define OP_SET_DIO_IRQ       0x08
#define OP_GET_IRQ_STATUS    0x12
#define OP_CLR_IRQ_STATUS    0x02
#define OP_GET_ERROR         0x17
#define OP_SET_FALLBACK      0x93

#define IRQ_TX_DONE       BIT(0)
#define IRQ_RX_DONE       BIT(1)
#define IRQ_PREAMBLE      BIT(2)
#define IRQ_SYNCWORD      BIT(3)
#define IRQ_HEADER_VALID  BIT(4)
#define IRQ_CAD_DONE      BIT(7)
#define IRQ_RX_TX_TIMEOUT BIT(9)

/* Status byte chip modes */
#define MODE_SLEEP     0
#define MODE_STBY_RC   2
#define MODE_STBY_XOSC 3
#define MODE_FS        4
#define MODE_RX        5
#define MODE_TX        6

#define PKT_GFSK 0
#define PKT_LORA 1

#define REG_GFSK_SYNC     0x06c0
#define REG_LORA_PAY_LEN  0x0702
#define REG_LORA_SYNC_TO  0x0706
#define REG_LORA_SYNCWORD 0x0740
#define REG_RANDOM        0x0819

#define RX_CONTINUOUS   0xffffff
#define QUEUE_LEN       8
#define TX_HISTORY      8
#define NOISE_DBM       (-120)
/* Host time to wait for the peer's answer to a transmission. */
#define VERDICT_WAIT_MS 2000

#define DIO1_PORT DEVICE_DT_GET(DT_GPIO_CTLR(DT_DRV_INST(0), dio1_gpios))
#define DIO1_PIN  DT_INST_GPIO_PIN(0, dio1_gpios)

enum event {
	EV_NONE,
	EV_TX_DONE,
	EV_RX_DONE,
	EV_RX_TIMEOUT,
	EV_CAD_DONE,
};

struct rx_entry {
	bool used;
	uint64_t due_us; /* start of its transmission, node uptime */
	struct sim_air_frame f;
};

static struct {
	uint8_t regs[0x1000];
	uint8_t buf[256];
	uint8_t mode;
	uint8_t fallback;
	uint8_t pkt_type;
	uint32_t freq_hz;
	/* LoRa */
	uint8_t sf, bw_code, cr, ldro;
	uint16_t preamble;
	uint8_t implicit, payload_len, crc_on, iq_inv;
	/* GFSK */
	uint32_t bitrate, fdev_hz;
	uint16_t gfsk_preamble_bits;
	uint8_t gfsk_sync_bits, gfsk_variable, gfsk_crc;
	int8_t power_dbm;
	uint8_t tx_base, rx_base;
	uint16_t irq_mask, dio1_mask, irq;
	uint8_t rx_len;
	int16_t rssi_dbm;
	int8_t snr_db;
	/* RX window */
	bool rx_continuous;
	uint64_t rx_end_us;
	int rx_slot;
	enum event ev;
	/* air */
	bool air_up;
	bool peer_alive;
	uint32_t tx_seq;
	bool verdict_pending;
	struct {
		uint32_t seq;
		uint64_t end_us;
	} tx_hist[TX_HISTORY];
	struct rx_entry queue[QUEUE_LEN];
	uint32_t tx_count, rx_count, lost_count;
} m = {
	.mode = MODE_STBY_RC,
	.fallback = MODE_STBY_RC,
	.pkt_type = PKT_GFSK,
	.rx_slot = -1,
};

static struct k_spinlock m_lock;
static struct k_timer m_event_timer;
static struct k_timer m_poll_timer;
static const char *m_air_peer;
static bool m_air_off;

static uint64_t now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static uint32_t lora_bw_hz(uint8_t code)
{
	static const uint32_t bw[] = {7810,   15630, 31250, 62500, 125000, 250000,
				      500000, 0,     10420, 20830, 41670};

	return code < ARRAY_SIZE(bw) ? bw[code] : 125000;
}

static uint32_t lora_symbol_us(uint8_t sf, uint32_t bw_hz)
{
	return (uint32_t)(((uint64_t)1000000 << sf) / MAX(bw_hz, 1u));
}

/* Time on air (SX1261/2 datasheet 6.1.4 / 6.2.3), in us. */
static uint32_t airtime_us(const struct sim_air_frame *f)
{
	if (f->modem == SIM_AIR_LORA) {
		uint32_t tsym = lora_symbol_us(f->sf, f->bw_hz);
		bool de = tsym >= 16000; /* LDRO above 16 ms symbols */
		int32_t num =
			8 * f->len - 4 * f->sf + 28 + 16 * f->crc_on - 20 * f->implicit_header;
		int32_t den = 4 * (f->sf - 2 * de);
		int32_t nsym = 8 + MAX(DIV_ROUND_UP(MAX(num, 0), den) * (f->cr + 4), 0);

		/* preamble + 4.25 symbols, in quarter symbols */
		return (uint32_t)((((uint64_t)f->preamble * 4 + 17 + nsym * 4) * tsym) / 4);
	}

	uint32_t bits =
		f->preamble + 16 /* sync */ + 8 /* length */ + 8 * f->len + (f->crc_on ? 16 : 0);

	return (uint32_t)((uint64_t)bits * 1000000 / MAX(f->bitrate, 1u));
}

static uint8_t status_byte(void)
{
	return (m.mode << 4) | (2 << 1);
}

static void update_dio1(void)
{
	(void)gpio_emul_input_set(DIO1_PORT, DIO1_PIN, (m.irq & m.dio1_mask) ? 1 : 0);
}

static void raise_irq(uint16_t irq)
{
	m.irq |= irq & m.irq_mask;
	update_dio1();
}

static void fill_frame_params(struct sim_air_frame *f)
{
	f->freq_hz = m.freq_hz;
	f->power_dbm = m.power_dbm;
	if (m.pkt_type == PKT_LORA) {
		f->modem = SIM_AIR_LORA;
		f->sf = m.sf;
		f->bw_hz = lora_bw_hz(m.bw_code);
		f->cr = m.cr;
		f->iq_inverted = m.iq_inv;
		f->crc_on = m.crc_on;
		f->implicit_header = m.implicit;
		f->preamble = m.preamble;
		f->sync_word = sys_get_be16(&m.regs[REG_LORA_SYNCWORD]);
	} else {
		f->modem = SIM_AIR_GFSK;
		f->bitrate = m.bitrate;
		f->fdev_hz = m.fdev_hz;
		f->crc_on = m.gfsk_crc != 0x01; /* RADIO_CRC_OFF = 0x01 */
		f->preamble = m.gfsk_preamble_bits;
		f->sync_word = sys_get_be16(&m.regs[REG_GFSK_SYNC]);
	}
}

static bool frame_matches(const struct sim_air_frame *f)
{
	struct sim_air_frame rx = {0};

	fill_frame_params(&rx);
	if (f->modem != rx.modem ||
	    (f->freq_hz > rx.freq_hz ? f->freq_hz - rx.freq_hz : rx.freq_hz - f->freq_hz) > 2000) {
		return false;
	}
	if (f->modem == SIM_AIR_LORA) {
		return f->sf == rx.sf && f->bw_hz == rx.bw_hz && f->iq_inverted == rx.iq_inverted &&
		       f->sync_word == rx.sync_word;
	}

	return f->bitrate == rx.bitrate && f->sync_word == rx.sync_word;
}

/* Latest a reception may start after the packet's start and still lock. */
static uint32_t lock_margin_us(const struct sim_air_frame *f)
{
	if (f->modem == SIM_AIR_LORA) {
		return MAX(f->preamble, 6u) * lora_symbol_us(f->sf, f->bw_hz) -
		       4 * lora_symbol_us(f->sf, f->bw_hz);
	}

	return (uint32_t)((uint64_t)f->preamble / 2 * 1000000 / MAX(f->bitrate, 1u));
}

static void handle_frame(const struct sim_air_frame *f, int len)
{
	if (len < SIM_AIR_HEADER_LEN || f->magic != SIM_AIR_MAGIC ||
	    f->version != SIM_AIR_VERSION) {
		return;
	}
	m.peer_alive = true;

	if (f->type == SIM_AIR_HELLO) {
		return;
	}
	if (m.verdict_pending && f->ref_seq == m.tx_seq) {
		m.verdict_pending = false;
	}
	if (f->type != SIM_AIR_PACKET || f->len > SIM_AIR_MAX_PAYLOAD ||
	    len < SIM_AIR_HEADER_LEN + f->len) {
		return;
	}

	uint64_t due = now_us();

	if (f->delay_us != SIM_AIR_ASAP) {
		for (int i = 0; i < TX_HISTORY; i++) {
			if (m.tx_hist[i].seq == f->ref_seq && m.tx_hist[i].end_us) {
				due = m.tx_hist[i].end_us + f->delay_us;
				break;
			}
		}
	}

	/* Free slot, else replace the oldest one. */
	int slot = 0;

	for (int i = 0; i < QUEUE_LEN; i++) {
		if (!m.queue[i].used) {
			slot = i;
			break;
		}
		if (m.queue[i].due_us < m.queue[slot].due_us) {
			slot = i;
		}
	}
	m.queue[slot].used = true;
	m.queue[slot].due_us = due;
	memcpy(&m.queue[slot].f, f, MIN((size_t)len, sizeof(*f)));
	LOG_DBG("air: queued %u B at %llu us (slot %d)", f->len, due, slot);
}

/* Read everything the peer has sent; wait up to timeout_ms (host time) for
 * the first datagram. Called without m_lock held when it may block. */
static void air_drain(int timeout_ms)
{
	struct sim_air_frame f;
	int n;

	if (!m.air_up) {
		return;
	}
	while ((n = sim_air_bottom_recv(&f, sizeof(f), timeout_ms)) > 0) {
		K_SPINLOCK(&m_lock) {
			handle_frame(&f, n);
		}
		timeout_ms = 0;
	}
}

static void schedule(enum event ev, uint64_t at_us)
{
	uint64_t now = now_us();

	m.ev = ev;
	k_timer_start(&m_event_timer, K_USEC(at_us > now ? at_us - now : 0), K_NO_WAIT);
}

/* Look for a queued packet this receive window catches; schedule its
 * reception, or the window's timeout. */
static void try_receive(void)
{
	uint64_t now = now_us();
	int best = -1;

	for (int i = 0; i < QUEUE_LEN; i++) {
		struct rx_entry *e = &m.queue[i];

		if (!e->used) {
			continue;
		}
		if (e->due_us + airtime_us(&e->f) + 10 * USEC_PER_SEC < now) {
			e->used = false; /* stale */
			continue;
		}
		if (!frame_matches(&e->f) || e->due_us + lock_margin_us(&e->f) < now ||
		    e->due_us > m.rx_end_us) {
			continue;
		}
		if (best < 0 || e->due_us < m.queue[best].due_us) {
			best = i;
		}
	}

	if (best >= 0) {
		m.rx_slot = best;
		k_timer_stop(&m_poll_timer);
		schedule(EV_RX_DONE, m.queue[best].due_us + airtime_us(&m.queue[best].f));
	} else if (m.rx_end_us != UINT64_MAX) {
		schedule(EV_RX_TIMEOUT, m.rx_end_us);
	} else {
		/* Open-ended: poll the air for packets sent meanwhile. */
		k_timer_start(&m_poll_timer, K_MSEC(5), K_MSEC(5));
	}
}

static void event_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	K_SPINLOCK(&m_lock) {
		enum event ev = m.ev;

		m.ev = EV_NONE;
		switch (ev) {
		case EV_TX_DONE:
			m.mode = m.fallback;
			raise_irq(IRQ_TX_DONE);
			break;
		case EV_RX_DONE: {
			struct rx_entry *e = &m.queue[m.rx_slot];

			memcpy(&m.buf[m.rx_base], e->f.data,
			       MIN(e->f.len, sizeof(m.buf) - m.rx_base));
			m.rx_len = e->f.len;
			m.regs[REG_LORA_PAY_LEN] = e->f.len;
			m.rssi_dbm = e->f.rssi_dbm;
			m.snr_db = e->f.snr_db;
			e->used = false;
			m.rx_slot = -1;
			m.rx_count++;
			if (!m.rx_continuous) {
				m.mode = m.fallback;
			}
			raise_irq(IRQ_RX_DONE |
				  (m.pkt_type == PKT_LORA ? (IRQ_PREAMBLE | IRQ_HEADER_VALID)
							  : (IRQ_PREAMBLE | IRQ_SYNCWORD)));
			if (m.rx_continuous) {
				try_receive();
			}
			break;
		}
		case EV_RX_TIMEOUT:
			m.mode = MODE_STBY_RC;
			raise_irq(IRQ_RX_TX_TIMEOUT);
			break;
		case EV_CAD_DONE:
			m.mode = MODE_STBY_RC;
			raise_irq(IRQ_CAD_DONE);
			break;
		default:
			break;
		}
	}
}

static void poll_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	air_drain(0);
	K_SPINLOCK(&m_lock) {
		if (m.mode == MODE_RX && m.ev == EV_NONE) {
			try_receive();
		}
	}
}

static void stop_activity(void)
{
	k_timer_stop(&m_event_timer);
	k_timer_stop(&m_poll_timer);
	m.ev = EV_NONE;
	m.rx_slot = -1;
}

static void cmd_set_tx(void)
{
	struct sim_air_frame f = {
		.magic = SIM_AIR_MAGIC,
		.version = SIM_AIR_VERSION,
		.type = SIM_AIR_PACKET,
	};
	uint8_t len = m.pkt_type == PKT_LORA ? m.payload_len : m.payload_len;

	stop_activity();
	fill_frame_params(&f);
	f.len = len;
	for (int i = 0; i < len; i++) {
		f.data[i] = m.buf[(uint8_t)(m.tx_base + i)];
	}

	uint32_t toa = airtime_us(&f);
	uint64_t end = now_us() + toa;

	f.seq = ++m.tx_seq;
	f.time_us = end;
	m.tx_hist[f.seq % TX_HISTORY].seq = f.seq;
	m.tx_hist[f.seq % TX_HISTORY].end_us = end;
	m.verdict_pending = m.air_up;
	m.tx_count++;
	m.mode = MODE_TX;

	LOG_DBG("TX %u B, %u Hz, airtime %u us", len, m.freq_hz, toa);
	if (m.air_up) {
		(void)sim_air_bottom_send(&f, SIM_AIR_HEADER_LEN + len);
	}
	schedule(EV_TX_DONE, end);
}

static void cmd_set_rx(uint32_t timeout)
{
	uint64_t now = now_us();

	stop_activity();
	m.mode = MODE_RX;
	m.rx_continuous = timeout == RX_CONTINUOUS;
	m.rx_end_us = UINT64_MAX;

	if (!m.rx_continuous && timeout != 0) {
		m.rx_end_us = now + (uint64_t)timeout * 15625 / 1000;
	}
	if (m.pkt_type == PKT_LORA && !m.rx_continuous) {
		/* LoRa symbol timeout: REG_LORA_SYNC_TO = exp + (mant << 3),
		 * symbols = 2 * mant * 4^exp (SX126xSetLoRaSymbNumTimeout). */
		uint8_t reg = m.regs[REG_LORA_SYNC_TO];
		uint32_t symbols = 2u * (reg >> 3) << (2 * (reg & 0x7));

		if (symbols > 0) {
			uint64_t end = now + (uint64_t)symbols *
						     lora_symbol_us(m.sf, lora_bw_hz(m.bw_code));

			m.rx_end_us = MIN(m.rx_end_us, end);
		}
	}
}

static uint8_t read_register(uint16_t addr)
{
	if (addr >= REG_RANDOM && addr < REG_RANDOM + 4) {
		return (uint8_t)sys_rand32_get();
	}

	return m.regs[addr & 0xfff];
}

/* One SPI transaction: tx[] in, rx[] out (same length). Returns true when a
 * receive window was opened (the caller then waits for the peer). */
static bool process(const uint8_t *tx, uint8_t *rx, size_t len)
{
	const uint8_t *p = &tx[1];
	size_t n = len - 1;
	bool rx_opened = false;

	memset(rx, 0, len);
	if (len == 0) {
		return false;
	}
	rx[0] = status_byte();
	if (len > 1) {
		rx[1] = status_byte();
	}

	/* Any command wakes the chip from sleep. */
	if (m.mode == MODE_SLEEP) {
		m.mode = MODE_STBY_RC;
	}

	switch (tx[0]) {
	case OP_WRITE_REGISTER:
		if (n >= 2) {
			uint16_t addr = sys_get_be16(p);

			for (size_t i = 2; i < n; i++) {
				m.regs[(addr + i - 2) & 0xfff] = p[i];
			}
		}
		break;
	case OP_READ_REGISTER:
		if (n >= 3) {
			uint16_t addr = sys_get_be16(p);

			for (size_t i = 4; i < len; i++) {
				rx[i] = read_register(addr + i - 4);
			}
		}
		break;
	case OP_WRITE_BUFFER:
		for (size_t i = 1; i < n; i++) {
			m.buf[(uint8_t)(p[0] + i - 1)] = p[i];
		}
		break;
	case OP_READ_BUFFER:
		for (size_t i = 3; i < len; i++) {
			rx[i] = m.buf[(uint8_t)(p[0] + i - 3)];
		}
		break;
	case OP_SET_SLEEP:
		stop_activity();
		m.mode = MODE_SLEEP;
		break;
	case OP_SET_STANDBY:
		stop_activity();
		m.mode = (n >= 1 && p[0]) ? MODE_STBY_XOSC : MODE_STBY_RC;
		break;
	case OP_SET_FS:
		stop_activity();
		m.mode = MODE_FS;
		break;
	case OP_SET_TX:
		cmd_set_tx();
		break;
	case OP_SET_RX:
		if (n >= 3) {
			cmd_set_rx(sys_get_be24(p));
			rx_opened = true;
		}
		break;
	case OP_SET_CAD:
		stop_activity();
		m.mode = MODE_RX;
		schedule(EV_CAD_DONE, now_us() + 4 * lora_symbol_us(m.sf, lora_bw_hz(m.bw_code)));
		break;
	case OP_SET_TX_CW:
	case OP_SET_TX_PREAMBLE:
		stop_activity();
		m.mode = MODE_TX;
		break;
	case OP_SET_PACKET_TYPE:
		if (n >= 1) {
			m.pkt_type = p[0];
		}
		break;
	case OP_GET_PACKET_TYPE:
		if (len > 2) {
			rx[2] = m.pkt_type;
		}
		break;
	case OP_SET_RF_FREQUENCY:
		if (n >= 4) {
			m.freq_hz = (uint32_t)(((uint64_t)sys_get_be32(p) * 32000000) >> 25);
		}
		break;
	case OP_SET_TX_PARAMS:
		if (n >= 1) {
			m.power_dbm = (int8_t)p[0];
		}
		break;
	case OP_SET_BUFFER_BASE:
		if (n >= 2) {
			m.tx_base = p[0];
			m.rx_base = p[1];
		}
		break;
	case OP_SET_MOD_PARAMS:
		if (m.pkt_type == PKT_LORA && n >= 4) {
			m.sf = p[0];
			m.bw_code = p[1];
			m.cr = p[2];
			m.ldro = p[3];
		} else if (m.pkt_type == PKT_GFSK && n >= 8) {
			uint32_t br = sys_get_be24(p);

			m.bitrate = br ? (uint32_t)(32ULL * 32000000 / br) : 0;
			m.fdev_hz = (uint32_t)(((uint64_t)sys_get_be24(&p[5]) * 32000000) >> 25);
		}
		break;
	case OP_SET_PKT_PARAMS:
		if (m.pkt_type == PKT_LORA && n >= 6) {
			m.preamble = sys_get_be16(p);
			m.implicit = p[2];
			m.payload_len = p[3];
			m.crc_on = p[4];
			m.iq_inv = p[5];
		} else if (m.pkt_type == PKT_GFSK && n >= 9) {
			m.gfsk_preamble_bits = sys_get_be16(p);
			m.gfsk_sync_bits = p[3];
			m.gfsk_variable = p[5];
			m.payload_len = p[6];
			m.gfsk_crc = p[7];
		}
		break;
	case OP_GET_RX_BUF_STATUS:
		if (len > 3) {
			rx[2] = m.rx_len;
			rx[3] = m.rx_base;
		}
		break;
	case OP_GET_PKT_STATUS:
		if (len > 4) {
			if (m.pkt_type == PKT_LORA) {
				rx[2] = (uint8_t)(-m.rssi_dbm * 2);
				rx[3] = (uint8_t)(m.snr_db * 4);
				rx[4] = (uint8_t)(-m.rssi_dbm * 2);
			} else {
				rx[2] = 0;
				rx[3] = (uint8_t)(-m.rssi_dbm * 2);
				rx[4] = (uint8_t)(-m.rssi_dbm * 2);
			}
		}
		break;
	case OP_GET_RSSI_INST:
		if (len > 2) {
			rx[2] = (uint8_t)(-NOISE_DBM * 2);
		}
		break;
	case OP_SET_DIO_IRQ:
		if (n >= 4) {
			m.irq_mask = sys_get_be16(p);
			m.dio1_mask = sys_get_be16(&p[2]);
			update_dio1();
		}
		break;
	case OP_GET_IRQ_STATUS:
		if (len > 3) {
			sys_put_be16(m.irq, &rx[2]);
		}
		break;
	case OP_CLR_IRQ_STATUS:
		if (n >= 2) {
			m.irq &= ~sys_get_be16(p);
			update_dio1();
		}
		break;
	case OP_SET_FALLBACK:
		if (n >= 1) {
			m.fallback = p[0] == 0x30   ? MODE_STBY_XOSC
				     : p[0] == 0x40 ? MODE_FS
						    : MODE_STBY_RC;
		}
		break;
	default:
		/* GetStatus, GetStats, GetDeviceErrors (all zero), calibration,
		 * regulator, TCXO, PA config, ...: nothing to model. */
		break;
	}

	return rx_opened;
}

static int emul_sx126x_io(const struct emul *target, const struct spi_config *config,
			  const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	uint8_t tx[300] = {0};
	uint8_t rx[300];
	size_t len = 0;
	bool rx_opened = false;

	ARG_UNUSED(target);
	ARG_UNUSED(config);

	/* Flatten the transfer: the driver splits command and data. */
	for (size_t i = 0; tx_bufs && i < tx_bufs->count; i++) {
		const struct spi_buf *b = &tx_bufs->buffers[i];
		size_t chunk = MIN(b->len, sizeof(tx) - len);

		if (b->buf) {
			memcpy(&tx[len], b->buf, chunk);
		}
		len += chunk;
	}
	if (rx_bufs) {
		size_t rx_len = 0;

		for (size_t i = 0; i < rx_bufs->count; i++) {
			rx_len += rx_bufs->buffers[i].len;
		}
		len = MAX(len, MIN(rx_len, sizeof(tx)));
	}

	K_SPINLOCK(&m_lock) {
		rx_opened = process(tx, rx, len);
	}

	if (rx_opened) {
		/* The first window after a transmission waits for the peer's
		 * answer to it, so it does not depend on host scheduling. */
		for (int waited = 0; m.verdict_pending && m.peer_alive && waited < VERDICT_WAIT_MS;
		     waited += 100) {
			air_drain(100);
		}
		air_drain(0);
		K_SPINLOCK(&m_lock) {
			m.verdict_pending = false;
			if (m.mode == MODE_RX && m.ev == EV_NONE) {
				try_receive();
			}
		}
	}

	if (rx_bufs) {
		size_t off = 0;

		for (size_t i = 0; i < rx_bufs->count; i++) {
			const struct spi_buf *b = &rx_bufs->buffers[i];
			size_t chunk = MIN(b->len, len - MIN(off, len));

			if (b->buf) {
				memcpy(b->buf, &rx[off], chunk);
			}
			off += b->len;
		}
	}

	return 0;
}

static struct spi_emul_api emul_sx126x_api = {
	.io = emul_sx126x_io,
};

static int emul_sx126x_init(const struct emul *target, const struct device *parent)
{
	struct sim_air_frame hello = {
		.magic = SIM_AIR_MAGIC,
		.version = SIM_AIR_VERSION,
		.type = SIM_AIR_HELLO,
	};

	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	k_timer_init(&m_event_timer, event_timer_fn, NULL);
	k_timer_init(&m_poll_timer, poll_timer_fn, NULL);
	sys_put_be16(0x1424, &m.regs[REG_LORA_SYNCWORD]); /* private network */

	if (m_air_off) {
		return 0;
	}

	/* The option parser leaves NULL when --air is not given. */
	if (m_air_peer == NULL) {
		m_air_peer = SIM_AIR_DEFAULT_PEER;
	}

	int port = sim_air_bottom_open(m_air_peer);

	if (port < 0) {
		LOG_WRN("air: cannot open (%d), radio is deaf", port);
		return 0;
	}
	m.air_up = true;
	(void)sim_air_bottom_send(&hello, SIM_AIR_HEADER_LEN);
	/* Learn whether a peer is there; it answers HELLO right away. */
	air_drain(300);
	LOG_INF("air: port %d, peer %s %s", port, m_air_peer,
		m.peer_alive ? "answered" : "not answering (yet)");

	return 0;
}

void sim_sx126x_status(uint32_t *freq_hz, uint8_t *mode, uint32_t *tx, uint32_t *rx, bool *peer)
{
	K_SPINLOCK(&m_lock) {
		*freq_hz = m.freq_hz;
		*mode = m.mode;
		*tx = m.tx_count;
		*rx = m.rx_count;
		*peer = m.peer_alive;
	}
}

static void sx126x_native_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "air",
		 .name = "host:port",
		 .type = 's',
		 .dest = (void *)&m_air_peer,
		 .descript = "UDP peer of the simulated radio (LoRaWAN network server / P2P "
			     "hub, scripts/sim/sim_air.py), default " SIM_AIR_DEFAULT_PEER},
		{.is_switch = true,
		 .option = "air_off",
		 .type = 'b',
		 .dest = (void *)&m_air_off,
		 .descript = "Simulated radio without the air: transmissions go nowhere"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(sx126x_native_options, PRE_BOOT_1, 1);

EMUL_DT_INST_DEFINE(0, emul_sx126x_init, NULL, NULL, &emul_sx126x_api, NULL);
