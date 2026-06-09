/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_history.h"
#include "app_config.h"
#include "app_sensor.h"

#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_HISTORY_HAVE_CLOCK 1
#endif

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_APP_HISTORY_FLASH)
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <time.h>
#endif

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_history, LOG_LEVEL_INF);

/* ---- Sensor descriptor table -------------------------------------------- */

enum hist_enc {
	ENC_TEMP, /* float -> int16 x100, sentinel 0x7FFF, 2 B */
	ENC_HUM,  /* float -> uint8 x2,   sentinel 0xFF,   1 B */
	ENC_COUNT /* uint32 absolute,                       4 B */
};

#define NO_CAP SIZE_MAX

struct hist_desc {
	const char *name;
	size_t src_off; /* offset in struct app_sensor_data */
	enum hist_enc enc;
	uint8_t size;
	size_t cap_off; /* offset of bool capability in struct app_config, or NO_CAP */
};

static const struct hist_desc m_desc[APP_HISTORY_SENSOR_COUNT] = {
	[APP_HISTORY_TEMPERATURE] = {"temperature", offsetof(struct app_sensor_data, temperature),
				     ENC_TEMP, 2, NO_CAP},
	[APP_HISTORY_HUMIDITY] = {"humidity", offsetof(struct app_sensor_data, humidity), ENC_HUM,
				  1, NO_CAP},
	[APP_HISTORY_EXT1] = {"ext1", offsetof(struct app_sensor_data, t1_temperature), ENC_TEMP, 2,
			      offsetof(struct app_config, cap_1w_thermometer)},
	[APP_HISTORY_EXT2] = {"ext2", offsetof(struct app_sensor_data, t2_temperature), ENC_TEMP, 2,
			      offsetof(struct app_config, cap_1w_thermometer)},
	[APP_HISTORY_MP1_TEMP] = {"mp1-temp", offsetof(struct app_sensor_data, mp1_temperature),
				  ENC_TEMP, 2, offsetof(struct app_config, cap_1w_machine_probe)},
	[APP_HISTORY_MP2_TEMP] = {"mp2-temp", offsetof(struct app_sensor_data, mp2_temperature),
				  ENC_TEMP, 2, offsetof(struct app_config, cap_1w_machine_probe)},
	[APP_HISTORY_MP1_HUM] = {"mp1-hum", offsetof(struct app_sensor_data, mp1_humidity), ENC_HUM,
				 1, offsetof(struct app_config, cap_1w_machine_probe)},
	[APP_HISTORY_MP2_HUM] = {"mp2-hum", offsetof(struct app_sensor_data, mp2_humidity), ENC_HUM,
				 1, offsetof(struct app_config, cap_1w_machine_probe)},
	[APP_HISTORY_HALL_LEFT] = {"hall-left", offsetof(struct app_sensor_data, hall_left_count),
				   ENC_COUNT, 4, offsetof(struct app_config, cap_hall_left)},
	[APP_HISTORY_HALL_RIGHT] = {"hall-right",
				    offsetof(struct app_sensor_data, hall_right_count), ENC_COUNT,
				    4, offsetof(struct app_config, cap_hall_right)},
	[APP_HISTORY_INPUT_A] = {"input-a", offsetof(struct app_sensor_data, input_a_count),
				 ENC_COUNT, 4, offsetof(struct app_config, cap_input_a)},
	[APP_HISTORY_INPUT_B] = {"input-b", offsetof(struct app_sensor_data, input_b_count),
				 ENC_COUNT, 4, offsetof(struct app_config, cap_input_b)},
	[APP_HISTORY_MOTION] = {"motion", offsetof(struct app_sensor_data, motion_count), ENC_COUNT,
				4, offsetof(struct app_config, cap_pir_detector)},
};

#define TEMP_SENTINEL   0x7FFF
#define HUM_SENTINEL    0xFF
#define MAX_RECORD_SIZE (13 * 4) /* worst case all channels, values only */

/* ---- Module state ------------------------------------------------------- */

static struct k_mutex m_lock;
static bool m_enabled;
static uint16_t m_mask; /* selected & available sensors */
static uint16_t m_sample_size;
static uint16_t m_capacity;
static uint16_t m_start; /* index of oldest record */
static uint16_t m_count;
static uint32_t m_base_time; /* oldest record's time: uptime-s unsynced, unix synced */
static bool m_base_synced;
static uint32_t m_interval; /* interval_report (s) the buffer was recorded at; records
			     * are periodic so per-record time = base + ord*interval */

static bool cap_on(size_t cap_off)
{
	if (cap_off == NO_CAP) {
		return true;
	}
	return *(const bool *)((const char *)&g_app_config + cap_off);
}

static uint32_t now_seconds(bool *synced)
{
	*synced = false;
#ifdef APP_HISTORY_HAVE_CLOCK
	uint32_t unix_s;
	if (app_clock_get_unix(&unix_s) == 0) {
		*synced = true;
		return unix_s;
	}
#endif
	return (uint32_t)(k_uptime_get() / 1000);
}

/* ---- Storage backend ---------------------------------------------------- */

#if defined(CONFIG_APP_HISTORY_FLASH)

#define NVS_ID_META     1
#define NVS_ID_REC_BASE 100

static struct nvs_fs m_fs;
static bool m_fs_ready;

struct hist_meta {
	uint32_t magic;
	uint16_t mask;
	uint16_t sample_size;
	uint16_t start;
	uint16_t count;
	uint32_t base_time;
	uint32_t interval;
	uint8_t base_synced;
};

#define META_MAGIC 0x48495332 /* "HIS2" — bumped: record layout dropped per-record delta */

static int backend_init(void)
{
	m_fs.flash_device = FIXED_PARTITION_DEVICE(history_partition);
	if (!device_is_ready(m_fs.flash_device)) {
		return -ENODEV;
	}
	m_fs.offset = FIXED_PARTITION_OFFSET(history_partition);

	struct flash_pages_info info;
	int ret = flash_get_page_info_by_offs(m_fs.flash_device, m_fs.offset, &info);
	if (ret) {
		return ret;
	}
	m_fs.sector_size = info.size;
	m_fs.sector_count = FIXED_PARTITION_SIZE(history_partition) / info.size;

	ret = nvs_mount(&m_fs);
	if (ret) {
		return ret;
	}
	m_fs_ready = true;
	return 0;
}

static void backend_save_meta(void)
{
	if (!m_fs_ready) {
		return;
	}
	struct hist_meta meta = {
		.magic = META_MAGIC,
		.mask = m_mask,
		.sample_size = m_sample_size,
		.start = m_start,
		.count = m_count,
		.base_time = m_base_time,
		.interval = m_interval,
		.base_synced = m_base_synced,
	};
	(void)nvs_write(&m_fs, NVS_ID_META, &meta, sizeof(meta));
}

/* Returns true if a valid meta with matching mask/sample_size was restored. */
static bool backend_load_meta(void)
{
	if (!m_fs_ready) {
		return false;
	}
	struct hist_meta meta;
	ssize_t r = nvs_read(&m_fs, NVS_ID_META, &meta, sizeof(meta));
	if (r != sizeof(meta) || meta.magic != META_MAGIC) {
		return false;
	}
	if (meta.mask != m_mask || meta.sample_size != m_sample_size) {
		return false; /* layout changed since last boot */
	}
	m_start = meta.start;
	m_count = meta.count;
	m_base_time = meta.base_time;
	m_interval = meta.interval;
	m_base_synced = meta.base_synced;
	return true;
}

static int backend_write_slot(uint16_t slot, const uint8_t *rec, size_t len)
{
	if (!m_fs_ready) {
		return -ENODEV;
	}
	ssize_t r = nvs_write(&m_fs, NVS_ID_REC_BASE + slot, rec, len);
	return (r < 0) ? (int)r : 0;
}

static int backend_read_slot(uint16_t slot, uint8_t *rec, size_t len)
{
	if (!m_fs_ready) {
		return -ENODEV;
	}
	ssize_t r = nvs_read(&m_fs, NVS_ID_REC_BASE + slot, rec, len);
	return (r == (ssize_t)len) ? 0 : -EIO;
}

static void backend_erase(void)
{
	if (!m_fs_ready) {
		return;
	}
	(void)nvs_clear(&m_fs);
	(void)nvs_mount(&m_fs);
}

static uint16_t backend_capacity(uint16_t sample_size)
{
	/* Reserve half the partition for NVS GC headroom; ~16 B overhead/record. */
	size_t usable = FIXED_PARTITION_SIZE(history_partition) / 2;
	size_t cap = usable / (sample_size + 16);
	return (uint16_t)MIN(cap, 4000);
}

#else /* RAM fallback */

static uint8_t __noinit m_ram[CONFIG_APP_HISTORY_BYTES];

static int backend_init(void)
{
	return 0;
}
static void backend_save_meta(void)
{
}
static bool backend_load_meta(void)
{
	return false; /* RAM ring starts empty each boot */
}
static int backend_write_slot(uint16_t slot, const uint8_t *rec, size_t len)
{
	memcpy(&m_ram[(size_t)slot * m_sample_size], rec, len);
	return 0;
}
static int backend_read_slot(uint16_t slot, uint8_t *rec, size_t len)
{
	memcpy(rec, &m_ram[(size_t)slot * m_sample_size], len);
	return 0;
}
static void backend_erase(void)
{
}
static uint16_t backend_capacity(uint16_t sample_size)
{
	return (uint16_t)(sizeof(m_ram) / sample_size);
}

#endif

/* ---- Mask / sizing ------------------------------------------------------ */

uint16_t app_history_available_mask(void)
{
	uint16_t m = 0;
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (cap_on(m_desc[i].cap_off)) {
			m |= BIT(i);
		}
	}
	return m;
}

static void recompute_sizing(void)
{
	uint16_t avail = app_history_available_mask();
	m_mask &= avail; /* drop sensors whose capability went away */

	uint16_t size = 0; /* values only; per-record time is implicit (base + ord*interval) */
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (m_mask & BIT(i)) {
			size += m_desc[i].size;
		}
	}
	m_sample_size = size;
	m_capacity = backend_capacity(size);
}

/* ---- Record codec ------------------------------------------------------- */

static size_t encode_value(uint8_t *p, int i)
{
	const struct hist_desc *d = &m_desc[i];
	const char *src = (const char *)&g_app_sensor_data + d->src_off;

	switch (d->enc) {
	case ENC_TEMP: {
		float f;
		memcpy(&f, src, sizeof(f));
		int16_t v = isnan(f) ? (int16_t)TEMP_SENTINEL : (int16_t)lroundf(f * 100.0f);
		sys_put_le16((uint16_t)v, p);
		return 2;
	}
	case ENC_HUM: {
		float f;
		memcpy(&f, src, sizeof(f));
		p[0] = isnan(f) ? HUM_SENTINEL : (uint8_t)lroundf(f * 2.0f);
		return 1;
	}
	case ENC_COUNT: {
		uint32_t c;
		memcpy(&c, src, sizeof(c));
		sys_put_le32(c, p);
		return 4;
	}
	}
	return 0;
}

static void decode_record(const uint8_t *rec, struct app_history_record *out)
{
	out->present = 0;
	const uint8_t *p = rec; /* values only — no per-record delta prefix */
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (!(m_mask & BIT(i))) {
			out->value[i] = 0;
			continue;
		}
		const struct hist_desc *d = &m_desc[i];
		switch (d->enc) {
		case ENC_TEMP: {
			int16_t v = (int16_t)sys_get_le16(p);
			if ((uint16_t)v != TEMP_SENTINEL) {
				out->value[i] = v / 100.0;
				out->present |= BIT(i);
			}
			p += 2;
			break;
		}
		case ENC_HUM: {
			uint8_t v = p[0];
			if (v != HUM_SENTINEL) {
				out->value[i] = v / 2.0;
				out->present |= BIT(i);
			}
			p += 1;
			break;
		}
		case ENC_COUNT: {
			uint32_t v = sys_get_le32(p);
			out->value[i] = (double)v;
			out->present |= BIT(i);
			p += 4;
			break;
		}
		}
	}
}

/* ---- Public API --------------------------------------------------------- */

void app_history_capture(void)
{
	if (!m_enabled || m_sample_size == 0 || m_capacity == 0) {
		return;
	}

	uint8_t rec[MAX_RECORD_SIZE];

	k_mutex_lock(&m_lock, K_FOREVER);

	/* Records are periodic at interval_report, so per-record time is implicit
	 * (base + ord*interval). If the interval changed, that timebase no longer
	 * holds — drop the history and restart at the new rate. */
	if (m_interval != (uint32_t)g_app_config.interval_report) {
		backend_erase();
		m_start = 0;
		m_count = 0;
		m_base_time = 0;
		m_base_synced = false;
		m_interval = (uint32_t)g_app_config.interval_report;
	}

	/* Snapshot sensor values atomically w.r.t. the sensor data. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	size_t pos = 0;
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (m_mask & BIT(i)) {
			pos += encode_value(&rec[pos], i);
		}
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	uint16_t slot;
	if (m_count < m_capacity) {
		if (m_count == 0) {
			bool synced;
			m_base_time = now_seconds(&synced);
			m_base_synced = synced;
		}
		slot = (m_start + m_count) % m_capacity;
		m_count++;
	} else {
		/* Full: evict oldest; the oldest ordinal moves forward one interval. */
		m_base_time += m_interval;
		slot = m_start;
		m_start = (m_start + 1) % m_capacity;
	}

	(void)backend_write_slot(slot, rec, m_sample_size);
	backend_save_meta();

	k_mutex_unlock(&m_lock);
}

void app_history_on_clock_sync(uint32_t unix_now)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	if (!m_base_synced && m_count > 0) {
		uint32_t off = unix_now - (uint32_t)(k_uptime_get() / 1000);
		m_base_time += off;
		m_base_synced = true;
		backend_save_meta();
	} else if (m_count == 0) {
		m_base_synced = true; /* next record will start absolute */
	}
	k_mutex_unlock(&m_lock);
}

size_t app_history_count(void)
{
	return m_count;
}

size_t app_history_capacity(void)
{
	return m_capacity;
}

void app_history_clear(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	backend_erase();
	m_start = 0;
	m_count = 0;
	m_base_time = 0;
	m_base_synced = false;
	backend_save_meta();
	k_mutex_unlock(&m_lock);
}

int app_history_get(size_t idx, struct app_history_record *out)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	if (idx >= m_count) {
		k_mutex_unlock(&m_lock);
		return -ENOENT;
	}

	/* Records are periodic: time = base + ord*interval (no per-record delta). */
	uint8_t rec[MAX_RECORD_SIZE];
	uint16_t slot = (m_start + idx) % m_capacity;
	(void)backend_read_slot(slot, rec, m_sample_size);
	decode_record(rec, out);
	out->time_unix = m_base_time + (uint32_t)idx * m_interval;
	out->time_synced = m_base_synced;

	k_mutex_unlock(&m_lock);
	return 0;
}

uint32_t app_history_get_interval(void)
{
	return m_interval;
}

/* Records have a fixed size (m_sample_size, values only) and a shared present
 * mask (m_mask) — so a wire frame carries the mask + interval once and each
 * record is just the raw stored bytes (sentinels mark absent values). Time is
 * implicit: ordinal `ord` is at base + ord*interval; records are time-ordered,
 * so the window filter can break once past `to_unix`. */
size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, uint16_t *n_written, size_t *next_ord)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	size_t pos = 0;
	uint16_t written = 0;
	uint32_t t0 = 0;
	bool have_t0 = false;
	size_t ord = start_ord;

	for (; ord < m_count; ord++) {
		uint32_t t = m_base_time + (uint32_t)ord * m_interval;
		if (m_base_synced) {
			if (t > to_unix) {
				break; /* monotonic time: no later record qualifies */
			}
			if (t < from_unix) {
				continue; /* not yet in window */
			}
		}
		if (pos + m_sample_size > cap) {
			break; /* this record spills to the next page */
		}
		uint16_t slot = (m_start + ord) % m_capacity;
		(void)backend_read_slot(slot, buf + pos, m_sample_size);
		if (!have_t0) {
			t0 = t;
			have_t0 = true;
		}
		pos += m_sample_size;
		written++;
	}

	if (t0_out) {
		*t0_out = t0;
	}
	if (n_written) {
		*n_written = written;
	}
	if (next_ord) {
		*next_ord = ord;
	}

	k_mutex_unlock(&m_lock);
	return pos;
}

/* Number of frames the [from,to] window needs at `cap` bytes/frame. Mirrors the
 * packing in export_page (fixed record size → whole records per frame). */
uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	if (m_sample_size == 0 || cap < m_sample_size) {
		return 0;
	}
	size_t per_frame = cap / m_sample_size;

	k_mutex_lock(&m_lock, K_FOREVER);
	size_t matched = 0;
	for (size_t ord = 0; ord < m_count; ord++) {
		uint32_t t = m_base_time + (uint32_t)ord * m_interval;
		if (m_base_synced) {
			if (t > to_unix) {
				break;
			}
			if (t < from_unix) {
				continue;
			}
		}
		matched++;
	}
	k_mutex_unlock(&m_lock);

	return (uint16_t)((matched + per_frame - 1) / per_frame);
}

bool app_history_is_enabled(void)
{
	return m_enabled;
}

void app_history_set_enabled(bool enable)
{
	m_enabled = enable;
}

uint16_t app_history_get_mask(void)
{
	return m_mask;
}

void app_history_set_mask(uint16_t mask)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint16_t avail = app_history_available_mask();
	m_mask = mask & avail;
	backend_erase();
	m_start = 0;
	m_count = 0;
	m_base_time = 0;
	m_base_synced = false;
	recompute_sizing();
	backend_save_meta();
	k_mutex_unlock(&m_lock);
}

const char *app_history_sensor_name(enum app_history_sensor s)
{
	if (s < 0 || s >= APP_HISTORY_SENSOR_COUNT) {
		return "?";
	}
	return m_desc[s].name;
}

enum app_history_sensor app_history_sensor_by_name(const char *name)
{
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (strcmp(name, m_desc[i].name) == 0) {
			return (enum app_history_sensor)i;
		}
	}
	return APP_HISTORY_SENSOR_COUNT;
}

bool app_history_sensor_available(enum app_history_sensor s)
{
	if (s < 0 || s >= APP_HISTORY_SENSOR_COUNT) {
		return false;
	}
	return cap_on(m_desc[s].cap_off);
}

int app_history_init(void)
{
	k_mutex_init(&m_lock);

	m_enabled = g_app_config.history_enable;

	/* Seed mask: explicit config bitmask, or "all available" when 0. */
	uint16_t avail = app_history_available_mask();
	uint16_t cfg = (uint16_t)g_app_config.history_sensors;
	m_mask = (cfg == 0) ? avail : (cfg & avail);

	int ret = backend_init();
	if (ret) {
		LOG_WRN("history backend init failed: %d (history unavailable)", ret);
		m_capacity = 0;
		return ret;
	}

	recompute_sizing();

	/* Restore prior buffer state if the layout matches; else start clean.
	 * m_interval = 0 forces the first capture to seed it from interval_report. */
	if (!backend_load_meta()) {
		m_start = 0;
		m_count = 0;
		m_base_time = 0;
		m_base_synced = false;
		m_interval = 0;
	}

	LOG_INF("history: enabled=%d, %u sensors, sample=%uB, capacity=%u, stored=%u", m_enabled,
		(unsigned)POPCOUNT(m_mask), m_sample_size, m_capacity, m_count);
	return 0;
}

/* ---- Shell -------------------------------------------------------------- */

#if defined(CONFIG_SHELL)

static const char *backend_name(void)
{
	return IS_ENABLED(CONFIG_APP_HISTORY_FLASH) ? "flash" : "ram";
}

static void print_value(const struct shell *sh, char *buf, size_t cap, int i, bool present,
			double v)
{
	if (!present) {
		snprintf(buf, cap, "--");
	} else if (m_desc[i].enc == ENC_COUNT) {
		snprintf(buf, cap, "%.0f", v);
	} else {
		snprintf(buf, cap, "%.2f", v);
	}
	ARG_UNUSED(sh);
}

static int cmd_history_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "enabled:   %s", m_enabled ? "yes" : "no");
	shell_print(sh, "backend:   %s", backend_name());

	char list[128] = "";
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (m_mask & BIT(i)) {
			if (list[0]) {
				strncat(list, ", ", sizeof(list) - strlen(list) - 1);
			}
			strncat(list, m_desc[i].name, sizeof(list) - strlen(list) - 1);
		}
	}
	shell_print(sh, "sensors:   %s (%u ch, %u B/record)", list[0] ? list : "(none)",
		    (unsigned)POPCOUNT(m_mask), m_sample_size);
	shell_print(sh, "capacity:  %u records", m_capacity);
	shell_print(sh, "stored:    %u / %u", m_count, m_capacity);
	shell_print(sh, "base:      %u (%s)", m_base_time,
		    m_base_synced ? "unix" : "uptime/no-rtc");
	return 0;
}

static int cmd_history_count(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "%u", (unsigned)app_history_count());
	return 0;
}

static int cmd_history_read(const struct shell *sh, size_t argc, char **argv)
{
	size_t count = app_history_count();
	size_t n = count;
	if (argc >= 2) {
		size_t req = (size_t)strtoul(argv[1], NULL, 10);
		if (req < n) {
			n = req;
		}
	}
	size_t first = count - n;

	/* Header */
	char hdr[160];
	int off = snprintf(hdr, sizeof(hdr), "%-5s %-20s", "#", "time");
	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT && off < (int)sizeof(hdr); i++) {
		if (m_mask & BIT(i)) {
			off += snprintf(hdr + off, sizeof(hdr) - off, " %8s", m_desc[i].name);
		}
	}
	shell_print(sh, "%s", hdr);

	for (size_t k = first; k < count; k++) {
		struct app_history_record r;
		if (app_history_get(k, &r) != 0) {
			break;
		}

		char tbuf[80];
		if (r.time_synced) {
			time_t t = (time_t)r.time_unix;
			struct tm tm;
			gmtime_r(&t, &tm);
			snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
				 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
				 tm.tm_min, tm.tm_sec);
		} else {
			snprintf(tbuf, sizeof(tbuf), "+%us (no-rtc)", r.time_unix - m_base_time);
		}

		char line[160];
		int lo = snprintf(line, sizeof(line), "%-5u %-20s", (unsigned)k, tbuf);
		for (int i = 0; i < APP_HISTORY_SENSOR_COUNT && lo < (int)sizeof(line); i++) {
			if (m_mask & BIT(i)) {
				char vb[16];
				print_value(sh, vb, sizeof(vb), i, r.present & BIT(i), r.value[i]);
				lo += snprintf(line + lo, sizeof(line) - lo, " %8s", vb);
			}
		}
		shell_print(sh, "%s", line);
	}
	return 0;
}

static int cmd_history_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	app_history_clear();
	shell_print(sh, "history cleared");
	return 0;
}

static int cmd_history_enable(const struct shell *sh, size_t argc, char **argv)
{
	bool en;
	if (strcmp(argv[1], "on") == 0) {
		en = true;
	} else if (strcmp(argv[1], "off") == 0) {
		en = false;
	} else {
		shell_error(sh, "usage: history enable on|off");
		return -EINVAL;
	}
	app_history_set_enabled(en);
	app_config()->history_enable = en; /* staged; persisted on `settings save` */
	shell_print(sh, "history %s", en ? "enabled" : "disabled");
	return 0;
}

static int cmd_history_sensors(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "%-12s %-7s %s", "sensor", "stored", "capability");
		for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
			shell_print(sh, "%-12s %-7s %s", m_desc[i].name,
				    (m_mask & BIT(i)) ? "yes" : "no",
				    app_history_sensor_available((enum app_history_sensor)i)
					    ? "available"
					    : "off");
		}
		return 0;
	}

	if (argc != 3) {
		shell_error(sh, "usage: history sensors [<name> on|off]");
		return -EINVAL;
	}

	enum app_history_sensor s = app_history_sensor_by_name(argv[1]);
	if (s == APP_HISTORY_SENSOR_COUNT) {
		shell_error(sh, "unknown sensor `%s`", argv[1]);
		return -EINVAL;
	}
	bool on = strcmp(argv[2], "on") == 0;
	if (!on && strcmp(argv[2], "off") != 0) {
		shell_error(sh, "usage: history sensors <name> on|off");
		return -EINVAL;
	}
	if (on && !app_history_sensor_available(s)) {
		shell_error(sh, "`%s` capability is off", argv[1]);
		return -EINVAL;
	}

	uint16_t mask = app_history_get_mask();
	mask = on ? (mask | BIT(s)) : (mask & ~BIT(s));
	app_history_set_mask(mask);
	app_config()->history_sensors = app_history_get_mask(); /* staged */
	shell_print(sh, "sensor `%s` %s; buffer cleared", argv[1], on ? "enabled" : "disabled");
	return 0;
}

static int cmd_history_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	size_t count = app_history_count();
	shell_print(sh, "%-12s %8s %8s %8s %5s", "sensor", "min", "max", "avg", "n");

	for (int i = 0; i < APP_HISTORY_SENSOR_COUNT; i++) {
		if (!(m_mask & BIT(i))) {
			continue;
		}
		double mn = 0, mx = 0, sum = 0;
		unsigned n = 0;
		for (size_t k = 0; k < count; k++) {
			struct app_history_record r;
			if (app_history_get(k, &r) != 0) {
				break;
			}
			if (!(r.present & BIT(i))) {
				continue;
			}
			double v = r.value[i];
			if (n == 0 || v < mn) {
				mn = v;
			}
			if (n == 0 || v > mx) {
				mx = v;
			}
			sum += v;
			n++;
		}
		if (n == 0) {
			shell_print(sh, "%-12s %8s %8s %8s %5u", m_desc[i].name, "--", "--", "--",
				    0);
		} else {
			shell_print(sh, "%-12s %8.2f %8.2f %8.2f %5u", m_desc[i].name, mn, mx,
				    sum / n, n);
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_history, SHELL_CMD_ARG(info, NULL, "Buffer summary.", cmd_history_info, 1, 0),
	SHELL_CMD_ARG(count, NULL, "Number of stored records.", cmd_history_count, 1, 0),
	SHELL_CMD_ARG(read, NULL, "List records. Usage: read [N]", cmd_history_read, 1, 1),
	SHELL_CMD_ARG(clear, NULL, "Erase the buffer.", cmd_history_clear, 1, 0),
	SHELL_CMD_ARG(sensors, NULL, "List/select sensors. Usage: sensors [<name> on|off]",
		      cmd_history_sensors, 1, 2),
	SHELL_CMD_ARG(enable, NULL, "Master on/off. Usage: enable on|off", cmd_history_enable, 2,
		      0),
	SHELL_CMD_ARG(stats, NULL, "Per-sensor min/max/avg.", cmd_history_stats, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(history, &sub_history, "Sensor history store-and-forward.", NULL);

#endif /* defined(CONFIG_SHELL) */
