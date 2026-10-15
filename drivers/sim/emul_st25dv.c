/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: ST25DV04K dynamic NFC tag on the emulated I2C bus, the part
 * of it app_nfc.c uses (DS10925):
 *   - E0 (0x53): 512 B user EEPROM, dynamic registers 0x2000..0x2007 and the
 *     256 B FTM mailbox RAM at 0x2008; E1 (0x57): the static system area and
 *     the I2C password at 0x0900 (both instances share one chip state).
 *   - The chip answers I2C only while powered from VCC: LPD (power-switch, high =
 *     off) low. Static writes need an open I2C security session (present
 *     password, default all-zero); every EEPROM write is NACKed while MB_EN=1.
 *   - Mailbox: an RF message sets RF_PUT + MB_LEN, the host reading its last
 *     byte clears RF_PUT; a host write to the RAM sets HOST_PUT + MB_LEN at STOP;
 *     clearing MB_EN drops both. MB_EN can be set only with MB_MODE=1 (static).
 *   - GPO (PB12) pulses on field change / RF mailbox put when GPO_EN and the
 *     event are enabled.
 * The RF side (the phone) is a UDP client on localhost, see sim_nfc_bottom.h and
 * scripts/sim/sim_nfc.py; `sim nfc` drives the field from the shell. */

#define DT_DRV_COMPAT hardwario_sim_st25dv

#include "sim.h"
#include "sim_nfc_bottom.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include "cmdline.h"
#include "soc.h"

LOG_MODULE_REGISTER(sim_st25dv, LOG_LEVEL_INF);

#define ADDR_E0 0x53
#define ADDR_E1 0x57

#define USER_SIZE   512
#define SYS_SIZE    0x24
#define REG_GPO     0x0000
#define REG_MB_MODE 0x000d
#define REG_PWD     0x0900

#define DYN_GPO_CTRL 0x2000
#define DYN_EH_CTRL  0x2002
#define DYN_RF_MNGT  0x2003
#define DYN_I2C_SSO  0x2004
#define DYN_IT_STS   0x2005
#define DYN_MB_CTRL  0x2006
#define DYN_MB_LEN   0x2007
#define MB_RAM       0x2008
#define MB_SIZE      256

#define GPO_EN           BIT(7)
#define GPO_RF_PUT_MSG   BIT(4)
#define GPO_FIELD_CHANGE BIT(3)
#define EH_FIELD_ON      BIT(2)
#define EH_VCC_ON        BIT(3)
#define MB_EN            BIT(0)
#define MB_HOST_PUT      BIT(1)
#define MB_RF_PUT        BIT(2)

#define POLL_MS 10

static const struct gpio_dt_spec m_lpd = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);
static const struct gpio_dt_spec m_gpo = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), nfc_gpo_gpios);

static struct {
	uint8_t user[USER_SIZE];
	uint8_t sys[SYS_SIZE];
	uint8_t pwd[8];
	bool sso;
	bool field;
	uint8_t gpo_dyn;
	uint8_t mb_ctrl;
	uint8_t mb_len; /* length - 1 */
	uint8_t mb[MB_SIZE];
	/* A phone transaction waiting for VCC / for the host's reply. */
	bool pending;
	uint8_t pending_msg[MB_SIZE];
	size_t pending_len;
	bool waiting_reply;
	uint32_t rf_puts;
	uint32_t host_puts;
} m;

static struct k_spinlock m_lock;
static struct k_timer m_poll_timer;
static bool m_up;
static int32_t m_port;
static bool m_nfc_off;

static bool vcc_on(void)
{
	/* LPD is a power switch, high = tag off; unconfigured reads as off. */
	return gpio_emul_output_get_dt(&m_lpd) == 0;
}

static void gpo_pulse(void)
{
	(void)gpio_emul_input_set_dt(&m_gpo, 1);
	(void)gpio_emul_input_set_dt(&m_gpo, 0);
}

/* GPO_CTRL_Dyn: GPO_EN is the only writable bit, the event enables mirror the
 * static GPO register. */
static bool gpo_event_enabled(uint8_t event)
{
	return (m.gpo_dyn & GPO_EN) && (m.sys[REG_GPO] & event);
}

static void dyn_reset(void)
{
	m.gpo_dyn = m.sys[REG_GPO];
	m.mb_ctrl = 0;
	m.mb_len = 0;
	m.sso = false;
}

static void send_status(void)
{
	uint8_t st[9] = {
		SIM_NFC_OP_STATUS, m.field,   vcc_on(),           m.mb_ctrl, m.mb_len,
		m.sys[REG_GPO],    m.gpo_dyn, m.sys[REG_MB_MODE], m.sso,
	};

	(void)sim_nfc_bottom_reply(st, sizeof(st));
}

static void send_error(uint8_t code)
{
	uint8_t e[2] = {SIM_NFC_OP_ERROR, code};

	(void)sim_nfc_bottom_reply(e, sizeof(e));
}

/* The phone's RF Write Dynamic Configuration (MB_EN=1) + Write Message, applied
 * once the chip runs on VCC (on field power alone MB_EN does not stick, as on
 * the bench). Returns true when the GPO must pulse. Called with the lock held. */
static bool pending_apply(void)
{
	if (!m.pending || !vcc_on()) {
		return false;
	}
	m.pending = false;

	if (!(m.sys[REG_MB_MODE] & 0x01)) {
		send_error(SIM_NFC_ERR_NO_FTM);
		return false;
	}
	m.mb_ctrl |= MB_EN;
	if (m.mb_ctrl & MB_RF_PUT) {
		send_error(SIM_NFC_ERR_BUSY);
		return false;
	}

	/* An unread host message is overwritten (the phone skipped it). */
	memcpy(m.mb, m.pending_msg, m.pending_len);
	m.mb_len = (uint8_t)(m.pending_len - 1);
	m.mb_ctrl = (m.mb_ctrl & ~MB_HOST_PUT) | MB_RF_PUT;
	m.waiting_reply = true;
	m.rf_puts++;

	return gpo_event_enabled(GPO_RF_PUT_MSG);
}

static bool field_set(bool on)
{
	if (m.field == on) {
		return false;
	}
	m.field = on;
	if (!on && !vcc_on()) {
		dyn_reset();
	}

	return gpo_event_enabled(GPO_FIELD_CHANGE);
}

void sim_st25dv_field(bool on)
{
	bool pulse;

	K_SPINLOCK(&m_lock) {
		pulse = field_set(on);
	}
	if (pulse) {
		gpo_pulse();
	}
}

void sim_st25dv_status(bool *field, bool *vcc, uint8_t *mb_ctrl, uint8_t *gpo_dyn,
		       uint32_t *rf_puts, uint32_t *host_puts)
{
	K_SPINLOCK(&m_lock) {
		*field = m.field;
		*vcc = vcc_on();
		*mb_ctrl = m.mb_ctrl;
		*gpo_dyn = m.gpo_dyn;
		*rf_puts = m.rf_puts;
		*host_puts = m.host_puts;
	}
}

static void poll_timer_fn(struct k_timer *timer)
{
	uint8_t buf[1 + MB_SIZE];
	bool pulse = false;
	int n;

	ARG_UNUSED(timer);

	K_SPINLOCK(&m_lock) {
		if (!m.field && !vcc_on()) {
			dyn_reset();
		}

		n = sim_nfc_bottom_recv(buf, sizeof(buf));
		if (n <= 0) {
			pulse = pending_apply();
			K_SPINLOCK_BREAK;
		}

		switch (buf[0]) {
		case SIM_NFC_OP_FIELD:
			pulse = field_set(n > 1 && buf[1]);
			m.waiting_reply = false;
			send_status();
			break;
		case SIM_NFC_OP_XFER:
			if (n < 2 || n - 1 > MB_SIZE) {
				send_error(SIM_NFC_ERR_LEN);
				break;
			}
			pulse = field_set(true);
			memcpy(m.pending_msg, &buf[1], n - 1);
			m.pending_len = n - 1;
			m.pending = true;
			pulse |= pending_apply();
			break;
		case SIM_NFC_OP_MB_OFF:
			m.mb_ctrl = 0;
			m.mb_len = 0;
			m.waiting_reply = false;
			send_status();
			break;
		default:
			send_status();
			break;
		}
	}

	if (pulse) {
		gpo_pulse();
	}
}

static uint8_t rd(int addr, uint16_t a)
{
	if (addr == ADDR_E1) {
		return a < SYS_SIZE ? m.sys[a] : 0;
	}
	if (a < USER_SIZE) {
		return m.user[a];
	}
	if (a >= MB_RAM && a < MB_RAM + MB_SIZE) {
		/* Reading the last byte of an RF message consumes it. */
		if ((m.mb_ctrl & MB_RF_PUT) && a - MB_RAM == m.mb_len) {
			m.mb_ctrl &= ~MB_RF_PUT;
		}
		return m.mb[a - MB_RAM];
	}

	switch (a) {
	case DYN_GPO_CTRL:
		return m.gpo_dyn;
	case DYN_EH_CTRL:
		return (m.field ? EH_FIELD_ON : 0) | (vcc_on() ? EH_VCC_ON : 0);
	case DYN_I2C_SSO:
		return m.sso;
	case DYN_MB_CTRL:
		return m.mb_ctrl;
	case DYN_MB_LEN:
		return m.mb_len;
	default:
		return 0;
	}
}

static int wr(int addr, uint16_t a, const uint8_t *data, size_t len)
{
	if (addr == ADDR_E1) {
		if (a == REG_PWD) {
			/* pwd(8) || 0x09 (present) || pwd(8) */
			if (len != 17 || data[8] != 0x09 || memcmp(data, &data[9], 8) != 0) {
				return -EIO;
			}
			m.sso = memcmp(data, m.pwd, 8) == 0;
			return 0;
		}
		if (!m.sso || (m.mb_ctrl & MB_EN) || a + len > SYS_SIZE) {
			return -EIO;
		}
		memcpy(&m.sys[a], data, len);
		return 0;
	}

	if (a < USER_SIZE) {
		if ((m.mb_ctrl & MB_EN) || a + len > USER_SIZE) {
			return -EIO;
		}
		memcpy(&m.user[a], data, len);
		return 0;
	}

	if (a == MB_RAM) {
		/* A host message: one write from the RAM start, MB_EN on, mailbox free. */
		if (!(m.mb_ctrl & MB_EN) || (m.mb_ctrl & (MB_RF_PUT | MB_HOST_PUT)) || len == 0 ||
		    len > MB_SIZE) {
			return -EIO;
		}
		memcpy(m.mb, data, len);
		m.mb_len = (uint8_t)(len - 1);
		m.mb_ctrl |= MB_HOST_PUT;
		m.host_puts++;

		if (m.waiting_reply && m.field) {
			uint8_t reply[1 + MB_SIZE];

			/* The phone reads it at once (its last byte clears HOST_PUT). */
			reply[0] = SIM_NFC_OP_REPLY;
			memcpy(&reply[1], data, len);
			(void)sim_nfc_bottom_reply(reply, 1 + len);
			m.mb_ctrl &= ~MB_HOST_PUT;
			m.waiting_reply = false;
		}
		return 0;
	}

	for (size_t i = 0; i < len; i++) {
		switch (a + i) {
		case DYN_GPO_CTRL:
			m.gpo_dyn = (data[i] & GPO_EN) | (m.sys[REG_GPO] & ~GPO_EN);
			break;
		case DYN_MB_CTRL:
			if (!(data[i] & MB_EN)) {
				m.mb_ctrl = 0;
				m.mb_len = 0;
			} else if (m.sys[REG_MB_MODE] & 0x01) {
				m.mb_ctrl |= MB_EN;
			}
			break;
		default:
			break; /* EH_CTRL / RF_MNGT: not modelled */
		}
	}

	return 0;
}

static int emul_st25dv_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	ARG_UNUSED(target);
	uint16_t a = 0;
	int ret = 0;
	bool pulse = false;

	K_SPINLOCK(&m_lock) {
		if (!vcc_on()) {
			ret = -EIO; /* unpowered: no ACK */
			K_SPINLOCK_BREAK;
		}
		pulse = pending_apply();

		for (int i = 0; i < num_msgs && ret == 0; i++) {
			struct i2c_msg *msg = &msgs[i];

			if (msg->flags & I2C_MSG_READ) {
				for (uint32_t j = 0; j < msg->len; j++) {
					msg->buf[j] = rd(addr, a++);
				}
			} else if (msg->len >= 2) {
				a = sys_get_be16(msg->buf);
				if (msg->len > 2) {
					ret = wr(addr, a, &msg->buf[2], msg->len - 2);
				}
			} else {
				ret = -EIO;
			}
		}
	}

	if (pulse) {
		gpo_pulse();
	}

	return ret;
}

static const struct i2c_emul_api emul_st25dv_api = {
	.transfer = emul_st25dv_transfer,
};

static int emul_st25dv_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	if (m_up) {
		return 0; /* both device addresses share one chip */
	}
	m_up = true;

	/* Factory state: GPO_EN + FIELD_CHANGE, FTM not authorised, password 0. */
	m.sys[REG_GPO] = GPO_EN | GPO_FIELD_CHANGE;
	dyn_reset();
	k_timer_init(&m_poll_timer, poll_timer_fn, NULL);

	if (m_nfc_off) {
		return 0;
	}

	/* The option parser leaves -1 when --nfc_port is not given. */
	if (m_port <= 0 || m_port > UINT16_MAX) {
		m_port = SIM_NFC_DEFAULT_PORT;
	}

	int port = sim_nfc_bottom_open(m_port);

	if (port < 0) {
		LOG_WRN("nfc: cannot bind 127.0.0.1:%d (%d), no phone link", m_port, port);
		return 0;
	}
	k_timer_start(&m_poll_timer, K_MSEC(POLL_MS), K_MSEC(POLL_MS));
	LOG_INF("nfc: phone link on 127.0.0.1:%d", port);

	return 0;
}

static void st25dv_native_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "nfc_port",
		 .name = "port",
		 .type = 'i',
		 .dest = (void *)&m_port,
		 .descript = "UDP port (127.0.0.1) of the simulated NFC phone link "
			     "(scripts/sim/sim_nfc.py), default " STRINGIFY(SIM_NFC_DEFAULT_PORT)},
		 {.is_switch = true,
		  .option = "nfc_off",
		  .type = 'b',
		  .dest = (void *)&m_nfc_off,
		  .descript = "No NFC phone link (the field is driven by `sim nfc` only)"},
		 ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(st25dv_native_options, PRE_BOOT_1, 1);

/* The application talks to the bus directly; each node still needs a device
 * for the emulator to bind to. */
#define ST25DV_EMUL(n)                                                                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,    \
			      NULL);                                                               \
	EMUL_DT_INST_DEFINE(n, emul_st25dv_init, NULL, NULL, &emul_st25dv_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(ST25DV_EMUL)
