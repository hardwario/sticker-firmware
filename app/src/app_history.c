/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_history.h"
#include "app_log.h"
#include "app_config.h"
#include "app_radio.h"
#include "app_sensor.h"

#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_HISTORY_HAVE_CLOCK 1
#endif

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_APP_HISTORY_FLASH)
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <time.h>
#endif

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_history, LOG_LEVEL_INF);

/* ---- Record layout ------------------------------------------------------ */

/* A record holds the channels listed in config.history_channels, in list order
 * (#430): entry `slot << 5 | ch` is channel ch of the slot's type (slot 0 = the
 * motherboard, 1..4 = 1-Wire sensorN with its sensorN_type). Encoding and scale
 * come from the registry (app_sensor_types.yaml `history`); the top value of an
 * encoding marks an absent value. An entry naming an unknown or retired channel,
 * a 1-Wire slot without a type, or a channel whose capability is off is
 * skipped; the layout is rebuilt (and the buffer restarted) when that changes. */

#define HIST_SLOT(e) ((uint8_t)((e) >> 5))
#define HIST_CH(e)   ((uint8_t)((e) & 0x1F))

struct hist_col {
	const struct app_sensor_channel *desc;
	uint8_t entry; /* slot << 5 | ch */
	uint8_t type;  /* enum app_sensor_type_id of the slot */
	uint8_t size;  /* encoded bytes */
};

#define MAX_RECORD_SIZE (APP_HISTORY_MAX_CH * 4) /* worst case: every column 4 B */

BUILD_ASSERT(SIZEOF_FIELD(struct app_config, history_channels) == APP_HISTORY_MAX_CH,
	     "history_channels size");
BUILD_ASSERT(APP_HISTORY_MAX_CH <= 32, "present mask is 32-bit");
BUILD_ASSERT(APP_W1_SLOT_COUNT <= 7 && APP_SENSOR_MB_CH_MAX <= 32, "entry is slot<<5|ch");

static const uint8_t m_enc_size[] = {
	[APP_SENSOR_HIST_U8] = 1,  [APP_SENSOR_HIST_I16] = 2, [APP_SENSOR_HIST_U16] = 2,
	[APP_SENSOR_HIST_I32] = 4, [APP_SENSOR_HIST_U32] = 4,
};

/* ---- Module state ------------------------------------------------------- */

static struct k_mutex m_lock;
static bool m_enabled;
static uint8_t m_list[APP_HISTORY_MAX_CH];         /* selection (config.history_channels) */
static struct hist_col m_cols[APP_HISTORY_MAX_CH]; /* recorded columns, record order */
static uint8_t m_ncols;
static uint8_t m_w1_types[APP_W1_SLOT_COUNT]; /* slot types the layout was built with */
static uint32_t m_layout;                     /* layout CRC, stamped into page headers */
/* Momentary columns record "asserted during the interval": the pulse counter
 * at the previous capture (bit i of m_pulses_known = m_pulses_prev[i] set). */
static uint32_t m_pulses_prev[APP_HISTORY_MAX_CH];
static uint32_t m_pulses_known;
static uint16_t m_sample_size;
static uint16_t m_capacity;
static uint16_t m_count;    /* logical record count (cached from the backend ring) */
static uint32_t m_interval; /* interval_report (s) the buffer was recorded at; records
			     * are periodic so per-record time = base + ord*interval */
/* True while app_radio_lrw streams a replay. Capture keeps running (the replay cursor
 * is absolute, see app_history_export_abs()); only the flash backend's page
 * rollover (a ~20 ms erase that stalls the CPU) is held off until it ends. */
static bool m_replay_active;

/* Record time is implicit and periodic, but only within a SEGMENT: a run of
 * consecutive records stamped from one base (flash: one page; RAM: an entry of
 * a 4-slot segment table).
 * The record at absolute ordinal `abs` of segment s is at
 *   s.base + (abs - s.origin) * m_interval
 * and a HistoryFrame never crosses a segment boundary, so the host's
 * t0 + j * interval_s stays exact without a protocol change. A new segment
 * starts with its own base from the clock, so a reboot / power loss becomes a
 * gap in the data instead of shifting every later timestamp (F28). */
struct hist_seg {
	uint32_t origin; /* absolute ordinal `base` refers to (<= first) */
	uint32_t first;  /* first record still stored */
	uint32_t end;    /* one past the last record */
	uint32_t base;   /* time of record `origin`: unix when synced, else uptime-s */
	bool synced;     /* base is unix time */
	bool cur_boot;   /* recorded during this boot (an uptime base is still valid) */
};

/* The ring self-persists: each record is durable once its double word flushes,
 * and page headers carry the base time / ordinal — so on reboot the count and
 * time base are recovered by scanning headers, no separate coalesced meta. A
 * clock-sync fixup of a page stamped before the RTC was set is persisted once in
 * the page's fix-up double word (flash backend), written from the report work
 * queue, so it survives the next reboot too. */

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

/* The backend owns the physical storage ring. The upper layer keeps only the
 * logical view (m_count, m_interval) and drives the ring through this API.
 * Records are addressed by ABSOLUTE ordinal: backend_first_abs() is the oldest
 * record, and an absolute ordinal names one record for as long as it is stored
 * (appends don't move it; eviction and logical resets only raise first_abs):
 *   backend_init()               probe the device
 *   backend_mount()              restore a prior ring (sets m_interval and the
 *                                segments); false = start empty
 *   backend_append(rec,len,base,synced,split,ev)
 *                                append one record; if it opens a new segment
 *                                (flash: page) that segment's base is
 *                                base/synced; `split` forces a new segment (a
 *                                slot discontinuity); *ev = records evicted
 *   backend_read(abs,rec,len)    read the record at absolute ordinal abs
 *   backend_stored()             current record count held by the ring
 *   backend_first_abs()          absolute ordinal of the oldest record
 *   backend_nseg(), backend_seg(i,s)
 *                                the segments, oldest first (struct hist_seg)
 *   backend_seg_sync(i,off)      re-base an unsynced segment of this boot by the
 *                                (unix - uptime) offset at clock sync (flash:
 *                                marks the page's fix-up double word pending)
 *   backend_flush_fixups()       write pending fix-ups (never from the downlink
 *                                callback, #96; the report work queue runs it)
 *   backend_reset_logical()      drop all records without a full erase (layout
 *                                change); the next append starts a fresh run
 *   backend_erase()              wipe all storage (explicit `history clear`)
 *   backend_capacity(size)       max records for a given sample size
 * The backend reads m_sample_size / m_layout / m_interval directly (same
 * translation unit) to stamp page headers. */

#if defined(CONFIG_APP_HISTORY_FLASH)

/* Raw flash page-ring backend (#265). History is a strictly sequential,
 * fixed-record-size, append-only stream, so it bypasses NVS (whose per-entry
 * allocation table + half-partition GC reserve wasted ~85 % of the partition)
 * and writes packed records straight through the flash API.
 *
 * Layout: the partition is a ring of 2 KB erase pages. Each page opens with a
 * 40 B header (v2: magic, monotonic sequence number, layout = mask/sample_size/
 * interval, clock time of the page's first record, absolute ordinal of that
 * record, CRC — 32 B, then one fix-up double word), followed by densely packed
 * records. Records never cross a page boundary. Newest page = highest sequence
 * number; on mount we scan headers to find head/tail — no separate meta entry.
 * v1 pages (32 B header, no fix-up double word; firmware before v1.5.0) are
 * still mounted and read; new pages are always v2.
 *
 * Time: each page is one segment (struct hist_seg) — its header base_time is
 * the clock time of its first record (RTC when set, else uptime), NOT the
 * ordinal continuation of the page before it. After every boot the first
 * append opens a new page, so a reboot or power loss is a gap, not a shift of
 * every later record (F28). A page opened before the RTC was set (power loss:
 * no RTC until the network DeviceTimeAns) carries an uptime base with
 * base_synced=0; its fix-up double word stays erased at page open and is
 * programmed once, after the clock sync, with the (unix - uptime) offset, so
 * the page's unix times survive later reboots too. The header itself can't be
 * rewritten (NOR: no second program without an erase).
 *
 * Durability: STM32WL programs flash in 8 B double words, so records are staged
 * into 7 B data slices and flushed one double word at a time — 7 data bytes + a
 * non-erased frame byte (0xA5). A double word whose frame byte reads 0xFF is
 * unwritten (erased), which delimits the write head unambiguously regardless of
 * record content (a humidity NaN encodes to 0xFF, so an erased-pattern scan over
 * the raw data would be unsafe). At most the staged tail (< 7 B ≈ up to ~2
 * records) is lost on power failure. */

#define PAGE_MAGIC_V1    0x48524e47 /* "HRNG" — history ring, 32 B header */
#define PAGE_MAGIC_V2    0x48524e32 /* "HRN2" — 40 B header with a fix-up DW */
#define PAGE_SIZE        2048
#define DW_SIZE          8 /* flash program unit (double word) */
#define DW_DATA          7 /* payload bytes per double word (byte 7 = frame) */
#define FRAME_BYTE       0xA5
#define ERASED_BYTE      0xFF
#define FIXUP_MARK       0x5A /* fix-up DW written (any value but 0xFF) */
#define HIST_HDR_SIZE    32   /* common header (v1 and v2) */
#define HIST_HDR_SIZE_V2 (HIST_HDR_SIZE + DW_SIZE)
#define PAYLOAD_DW_V1    ((PAGE_SIZE - HIST_HDR_SIZE) / DW_SIZE)    /* 252 */
#define PAYLOAD_DW_V2    ((PAGE_SIZE - HIST_HDR_SIZE_V2) / DW_SIZE) /* 251 */
#define PAGE_DATA_V1     (PAYLOAD_DW_V1 * DW_DATA)                  /* 1764 B / page */
#define PAGE_DATA_V2     (PAYLOAD_DW_V2 * DW_DATA)                  /* 1757 B / page */
#define HIST_NPAGES      (FIXED_PARTITION_SIZE(history_partition) / PAGE_SIZE)

BUILD_ASSERT(HIST_NPAGES >= 3, "history partition too small for a page ring");
/* Worst case (1 B sample, all pages v1) record count must fit the uint16_t
 * logical count. */
BUILD_ASSERT((uint32_t)HIST_NPAGES *PAGE_DATA_V1 <= UINT16_MAX,
	     "history ring exceeds uint16 count");

struct hist_page_hdr {
	uint32_t magic;
	uint32_t seq;         /* monotonic; newest page has the highest seq */
	uint32_t layout;      /* layout guard: CRC of the current columns (#430) */
	uint32_t interval;    /* seconds between records when the page was written */
	uint32_t base_time;   /* clock time of this page's first record */
	uint32_t first_ord;   /* absolute ordinal of this page's first record */
	uint16_t sample_size; /* layout guard */
	uint8_t base_synced;  /* 1 if base_time was a synced unix time */
	uint8_t rsv;
	uint16_t crc; /* crc16-ccitt over the preceding 30 bytes */
	uint16_t rsv2;
} __packed;

BUILD_ASSERT(sizeof(struct hist_page_hdr) == HIST_HDR_SIZE, "page header must be 32 B");

/* v2 only: the double word right after the header. Erased (0xFF) until the page,
 * opened with an uptime base, learns the unix time; then programmed once. */
struct hist_page_fixup {
	uint32_t offset; /* unix - uptime (s): add to the header base_time */
	uint8_t mark;    /* FIXUP_MARK */
	uint8_t rsv;     /* 0 */
	uint16_t crc;    /* crc16-ccitt over offset..rsv, seeded with the header crc */
} __packed;

BUILD_ASSERT(sizeof(struct hist_page_fixup) == DW_SIZE, "fix-up must be one double word");

static const struct flash_area *m_fa;
static bool m_ready;

/* Every history program/erase goes through these, clear of any radio exchange:
 * the flash stall would land a TX's receive windows late (app_radio_flash_hold()). */
static int hist_flash_write(off_t off, const void *data, size_t len)
{
	app_radio_flash_hold();
	int ret = flash_area_write(m_fa, off, data, len);
	app_radio_flash_release();
	return ret;
}

static int hist_flash_erase(off_t off, size_t len)
{
	app_radio_flash_hold();
	int ret = flash_area_erase(m_fa, off, len);
	app_radio_flash_release();
	return ret;
}

/* In-RAM ring state (reconstructed on mount, maintained on append). */
struct live_page {
	uint32_t first_ord; /* absolute ordinal of the page's first record */
	uint32_t base_time; /* time of that record (header base, clock-sync fixed) */
	uint16_t phys;      /* physical page index in the partition */
	uint8_t flags;      /* LP_* */
};
#define LP_SYNCED   BIT(0) /* base_time is unix time */
#define LP_CUR_BOOT BIT(1) /* opened during this boot (an uptime base is valid) */
#define LP_V2       BIT(2) /* 40 B header with a fix-up double word */
#define LP_FIXUP    BIT(3) /* fix-up double word still to be programmed */

static struct live_page m_live[HIST_NPAGES]; /* [0] = tail (oldest) .. [n-1] = head */
static uint16_t m_nlive;
static uint32_t m_next_seq;      /* seq to assign to the next new page */
static uint32_t m_abs_ord;       /* absolute ordinal of the next record to append */
static uint16_t m_last_phys;     /* last physical page allocated (for ring progression) */
static uint16_t m_head_dw;       /* next payload double word to write in the head page */
static uint8_t m_stage[DW_DATA]; /* staged data bytes not yet in a full double word */
static uint8_t m_stage_len;
static bool m_head_full; /* head page cannot take more records → next append rolls over */

static off_t page_off(uint16_t phys)
{
	return (off_t)phys * PAGE_SIZE;
}

/* Records per page: new pages are always v2. */
static uint16_t records_per_page(uint16_t sample_size)
{
	return sample_size ? (uint16_t)(PAGE_DATA_V2 / sample_size) : 0;
}

/* Offset of the record stream within a page of the given header version. */
static off_t page_data_off(bool v2)
{
	return v2 ? HIST_HDR_SIZE_V2 : HIST_HDR_SIZE;
}

/* Head-page live record count = ordinals since the head page's first record. */
static uint16_t head_records(void)
{
	if (m_nlive == 0) {
		return 0;
	}
	return (uint16_t)(m_abs_ord - m_live[m_nlive - 1].first_ord);
}

static void hdr_crc_set(struct hist_page_hdr *h)
{
	h->crc = crc16_ccitt(0xffff, (const uint8_t *)h, offsetof(struct hist_page_hdr, crc));
}

static bool hdr_valid(const struct hist_page_hdr *h)
{
	if (h->magic != PAGE_MAGIC_V1 && h->magic != PAGE_MAGIC_V2) {
		return false;
	}
	uint16_t crc = crc16_ccitt(0xffff, (const uint8_t *)h, offsetof(struct hist_page_hdr, crc));
	return crc == h->crc;
}

static int read_hdr(uint16_t phys, struct hist_page_hdr *h)
{
	return flash_area_read(m_fa, page_off(phys), h, sizeof(*h));
}

/* A v2 page's clock-sync fix-up: true (and *base = unix base) when written. */
static bool fixup_read(uint16_t phys, const struct hist_page_hdr *h, uint32_t *base)
{
	struct hist_page_fixup f;

	if (flash_area_read(m_fa, page_off(phys) + HIST_HDR_SIZE, &f, sizeof(f)) != 0 ||
	    f.mark != FIXUP_MARK) {
		return false;
	}
	if (crc16_ccitt(h->crc, (const uint8_t *)&f, offsetof(struct hist_page_fixup, crc)) !=
	    f.crc) {
		return false;
	}
	*base = h->base_time + f.offset;
	return true;
}

/* Read `len` data-stream bytes starting at data offset `off` within live page
 * `lp`, skipping the per-double-word frame byte and pulling the still-staged
 * tail of the head page from RAM. */
static int page_read_stream(const struct live_page *lp, size_t off, uint8_t *dst, size_t len)
{
	uint16_t phys = lp->phys;
	bool is_head = (m_nlive > 0 && phys == m_live[m_nlive - 1].phys);
	size_t flushed = (size_t)m_head_dw * DW_DATA;
	off_t data = page_off(phys) + page_data_off(lp->flags & LP_V2);

	for (size_t j = 0; j < len; j++) {
		size_t d = off + j;
		if (is_head && d >= flushed) {
			size_t s = d - flushed;
			if (s >= m_stage_len) {
				return -EIO; /* past the write head */
			}
			dst[j] = m_stage[s];
			continue;
		}
		size_t dw = d / DW_DATA;
		size_t b = d % DW_DATA;
		int ret = flash_area_read(m_fa, data + dw * DW_SIZE + b, &dst[j], 1);
		if (ret) {
			return ret;
		}
	}
	return 0;
}

/* Flush any staged bytes as one padded double word (page finalize). #384: must
 * propagate a write failure — the caller (advance_page()) is documented to leave
 * all in-RAM ring state untouched on error so the operation can be retried; if
 * this write silently "succeeded" from the caller's point of view, m_head_dw/
 * m_stage_len would advance past a page whose last bytes were never actually
 * committed to flash, and a later read would return corrupted/uninitialized
 * data for those bytes (same class of bug backend_append() already guards
 * against, C1). */
static int flush_stage_pad(void)
{
	if (m_stage_len == 0) {
		return 0;
	}
	uint8_t dw[DW_SIZE];
	memset(dw, 0, DW_DATA);
	memcpy(dw, m_stage, m_stage_len);
	dw[DW_DATA] = FRAME_BYTE;
	const struct live_page *head = &m_live[m_nlive - 1];
	int ret = hist_flash_write(page_off(head->phys) + page_data_off(head->flags & LP_V2) +
					   (off_t)m_head_dw * DW_SIZE,
				   dw, DW_SIZE);
	if (ret) {
		return ret;
	}
	m_head_dw++;
	m_stage_len = 0;
	return 0;
}

/* Roll over to a fresh head page stamped `base`/`synced` (the clock time of the
 * record about to open it), evicting the tail page if the ring wraps onto it.
 * On success (0 returned) *evicted holds the number of records evicted and all
 * in-RAM ring state (m_live[], m_nlive, m_next_seq, m_last_phys, m_head_dw,
 * m_stage_len, m_head_full) reflects the new head page. On failure, the flash
 * erase/write itself may or may not have partially completed, but no in-RAM
 * state is mutated — the caller can safely retry on the next append (the
 * candidate physical page is not yet claimed as live). */
static int advance_page(uint32_t base, bool synced, uint32_t *evicted)
{
	*evicted = 0;

	/* Durably close the current head so its committed records survive. #384: a
	 * failure here must abort the rollover (return early, no in-RAM state
	 * mutated yet) rather than proceeding to claim a new head page while the
	 * old one's tail bytes were never actually flushed. */
	if (m_nlive > 0) {
		int ret = flush_stage_pad();
		if (ret) {
			return ret;
		}
	}

	uint16_t next = (uint16_t)((m_last_phys + 1) % HIST_NPAGES);

	bool will_evict = (m_nlive > 0 && next == m_live[0].phys);
	uint32_t tail_recs = 0;
	if (will_evict) {
		/* Ring wraps onto the oldest page — compute (but don't yet commit) its
		 * eviction. */
		tail_recs = (m_nlive > 1) ? (m_live[1].first_ord - m_live[0].first_ord)
					  : (m_abs_ord - m_live[0].first_ord);
	}

	int ret = hist_flash_erase(page_off(next), PAGE_SIZE);
	if (ret) {
		return ret;
	}

	/* v2: the 32 B common header; the fix-up double word after it stays erased
	 * until a clock sync re-bases an uptime-stamped page. */
	struct hist_page_hdr h = {
		.magic = PAGE_MAGIC_V2,
		.seq = m_next_seq,
		.layout = m_layout,
		.interval = m_interval,
		.base_time = base,
		.first_ord = m_abs_ord,
		.sample_size = m_sample_size,
		.base_synced = synced ? 1 : 0,
	};
	hdr_crc_set(&h);
	ret = hist_flash_write(page_off(next), &h, sizeof(h));
	if (ret) {
		return ret;
	}

	/* Both the erase and header write landed — commit the new page. */
	if (will_evict) {
		*evicted = tail_recs;
		for (uint16_t i = 1; i < m_nlive; i++) {
			m_live[i - 1] = m_live[i];
		}
		m_nlive--;
	}
	m_next_seq++;
	m_last_phys = next;
	m_live[m_nlive] = (struct live_page){
		.first_ord = m_abs_ord,
		.base_time = base,
		.phys = next,
		.flags = (synced ? LP_SYNCED : 0) | LP_CUR_BOOT | LP_V2,
	};
	m_nlive++;
	m_head_dw = 0;
	m_stage_len = 0;
	m_head_full = false;
	return 0;
}

static int backend_init(void)
{
	if (flash_area_open(FIXED_PARTITION_ID(history_partition), &m_fa) != 0) {
		return -ENODEV;
	}
	const struct device *dev = flash_area_get_device(m_fa);
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	struct flash_pages_info info;
	int ret =
		flash_get_page_info_by_offs(dev, FIXED_PARTITION_OFFSET(history_partition), &info);
	if (ret) {
		return ret;
	}
	if (info.size != PAGE_SIZE) {
		/* The ring assumes 2 KB erase pages (STM32WL). */
		LOG_ERR("history: flash page size %zu != %d", info.size, PAGE_SIZE);
		return -ENOTSUP;
	}
	m_ready = true;
	m_last_phys = (uint16_t)(HIST_NPAGES - 1); /* first advance() wraps to page 0 */
	return 0;
}

/* Count the written payload double words in a page (frame byte != 0xFF). */
static uint16_t scan_written_dw(uint16_t phys, bool v2)
{
	uint16_t dw = 0;
	uint16_t payload_dw = v2 ? PAYLOAD_DW_V2 : PAYLOAD_DW_V1;
	for (; dw < payload_dw; dw++) {
		uint8_t frame = ERASED_BYTE;
		if (flash_area_read(m_fa,
				    page_off(phys) + page_data_off(v2) + (off_t)dw * DW_SIZE +
					    DW_DATA,
				    &frame, 1) != 0) {
			break;
		}
		if (frame == ERASED_BYTE) {
			break;
		}
	}
	return dw;
}

static bool backend_mount(void)
{
	m_nlive = 0;
	m_abs_ord = 0;
	m_next_seq = 0;
	m_head_dw = 0;
	m_stage_len = 0;
	m_head_full = false;

	if (!m_ready || m_sample_size == 0) {
		return false;
	}

	/* Find the head = valid page with the highest sequence number whose layout
	 * (layout CRC + sample size) matches the current selection. */
	bool have_head = false;
	uint16_t head_phys = 0;
	struct hist_page_hdr head_hdr = {0};

	for (uint16_t p = 0; p < HIST_NPAGES; p++) {
		struct hist_page_hdr h;
		if (read_hdr(p, &h) != 0 || !hdr_valid(&h)) {
			continue;
		}
		if (h.layout != m_layout || h.sample_size != m_sample_size) {
			continue;
		}
		if (!have_head || (int32_t)(h.seq - head_hdr.seq) > 0) {
			have_head = true;
			head_phys = p;
			head_hdr = h;
		}
	}
	if (!have_head) {
		return false;
	}

	/* Walk physically backward from the head, collecting the contiguous run of
	 * pages that share the head's interval and decreasing sequence numbers. A
	 * gap (interval change, evicted page, older layout) ends the live set. */
	uint16_t chain[HIST_NPAGES];
	uint16_t chain_len = 0;
	chain[chain_len++] = head_phys;
	struct hist_page_hdr cur = head_hdr;

	for (uint16_t i = 1; i < HIST_NPAGES; i++) {
		uint16_t prev = (uint16_t)((head_phys + HIST_NPAGES - i) % HIST_NPAGES);
		struct hist_page_hdr h;
		if (read_hdr(prev, &h) != 0 || !hdr_valid(&h)) {
			break;
		}
		if (h.layout != m_layout || h.sample_size != m_sample_size ||
		    h.interval != head_hdr.interval || (uint32_t)(cur.seq - h.seq) != 1) {
			break;
		}
		chain[chain_len++] = prev;
		cur = h;
	}

	/* chain is head..tail; store as tail..head in m_live. Each page keeps its
	 * own header time base (plus its clock-sync fix-up, if one was written);
	 * none is from this boot. */
	for (uint16_t i = 0; i < chain_len; i++) {
		uint16_t phys = chain[chain_len - 1 - i];
		struct hist_page_hdr h;
		(void)read_hdr(phys, &h);
		bool v2 = (h.magic == PAGE_MAGIC_V2);
		uint32_t base = h.base_time;
		bool synced = h.base_synced != 0;

		if (v2 && !synced && fixup_read(phys, &h, &base)) {
			synced = true;
		}
		m_live[i] = (struct live_page){
			.first_ord = h.first_ord,
			.base_time = base,
			.phys = phys,
			.flags = (synced ? LP_SYNCED : 0) | (v2 ? LP_V2 : 0),
		};
	}
	m_nlive = chain_len;

	/* Recover the head page's committed record count by scanning its written
	 * double words. The staged tail (< 7 B) from before the reboot is gone. */
	bool head_v2 = (head_hdr.magic == PAGE_MAGIC_V2);
	uint16_t written = scan_written_dw(head_phys, head_v2);
	uint16_t rpp = (uint16_t)((head_v2 ? PAGE_DATA_V2 : PAGE_DATA_V1) / m_sample_size);
	uint16_t hrecs = (uint16_t)(((size_t)written * DW_DATA) / m_sample_size);
	if (hrecs > rpp) {
		hrecs = rpp;
	}
	m_abs_ord = head_hdr.first_ord + hrecs;
	m_next_seq = head_hdr.seq + 1;
	m_last_phys = head_phys;
	/* Do not append into the recovered head page (its tail double word may hold
	 * a partial record, and the device may have been off for any time since its
	 * last record); treat it as finalized and roll over on the next append, which
	 * stamps the new page from the clock. first_ord bookkeeping lets a short page
	 * carry fewer than rpp records. */
	m_head_full = true;
	m_head_dw = written;
	m_stage_len = 0;

	m_interval = head_hdr.interval; /* the whole chain shares it */

	return (m_abs_ord - m_live[0].first_ord) > 0;
}

static int backend_append(const uint8_t *rec, size_t len, uint32_t base, bool synced, bool split,
			  uint32_t *evicted)
{
	*evicted = 0;
	if (!m_ready) {
		return -ENODEV;
	}

	uint16_t rpp = records_per_page(m_sample_size);
	if (rpp == 0) {
		return -EINVAL;
	}

	/* A slot discontinuity (missed slots, clock step) closes the head page
	 * early: the record opens a new page stamped with its own time. The rest of
	 * the old page stays unused. */
	if (split && m_nlive > 0 && !m_head_full) {
		m_head_full = true;
		(void)flush_stage_pad(); /* retried by advance_page() on failure */
	}

	if (m_nlive == 0 || m_head_full || (head_records() + 1) > rpp) {
		/* A rollover erases a page (~20 ms CPU stall). Never inside a replay:
		 * the stall could land in its RX windows. Writes within the current page
		 * go on; a record that needs the next page is dropped (there is no RAM
		 * for a deferred-record queue) and the next capture after the replay
		 * opens the page. */
		if (m_replay_active) {
			return -EBUSY;
		}
		uint32_t adv_evicted = 0;
		int ret = advance_page(base, synced, &adv_evicted);
		if (ret) {
			return ret;
		}
		*evicted += adv_evicted;
	}

	/* Stream `len` bytes into the head page (always v2: pages recovered at
	 * mount are closed), flushing full double words. */
	off_t data = page_off(m_live[m_nlive - 1].phys) + HIST_HDR_SIZE_V2;
	uint16_t start_head_dw = m_head_dw;
	uint8_t start_stage_len = m_stage_len;
	for (size_t i = 0; i < len; i++) {
		m_stage[m_stage_len++] = rec[i];
		if (m_stage_len == DW_DATA) {
			uint8_t dw[DW_SIZE];
			memcpy(dw, m_stage, DW_DATA);
			dw[DW_DATA] = FRAME_BYTE;
			int ret = hist_flash_write(data + (off_t)m_head_dw * DW_SIZE, dw, DW_SIZE);
			/* Drop the staged double word on error too -- leaving m_stage_len
			 * stuck at DW_DATA would run this loop's next byte past the end of
			 * m_stage[] on the following capture (C1). */
			m_stage_len = 0;
			if (ret) {
				/* A failure past the record's first byte leaves a gap in the
				 * page's byte stream: this record's already-flushed double
				 * words (m_head_dw advanced past them while m_abs_ord never
				 * counted the record), and/or the previous record's staged
				 * tail bytes just dropped above. Every later record in this
				 * page would then read back shifted/garbage. NOR flash cannot
				 * be rewound, so contain the damage: close the page, forcing
				 * the next append onto a fresh one whose stream starts
				 * aligned again. A failure with nothing flushed and an empty
				 * pre-record stage leaves no gap -- keep the page open. */
				if (m_head_dw != start_head_dw || start_stage_len != 0) {
					m_head_full = true;
				}
				return ret;
			}
			m_head_dw++;
		}
	}
	m_abs_ord++;
	if (head_records() >= rpp) {
		m_head_full = true;
		/* Commit the full page's staged tail now rather than at the next
		 * rollover (1757 B pages leave e.g. 5 B of 3 B records staged), so a
		 * reboot doesn't cost its last records. One double word program; a
		 * failure is retried by advance_page(). */
		(void)flush_stage_pad();
	}
	return 0;
}

static int backend_read(uint32_t abs, uint8_t *rec, size_t len)
{
	if (!m_ready || m_nlive == 0) {
		return -EIO;
	}
	if (abs < m_live[0].first_ord || abs >= m_abs_ord) {
		return -EIO;
	}
	uint16_t p = m_nlive - 1;
	while (p > 0 && m_live[p].first_ord > abs) {
		p--;
	}
	size_t local = abs - m_live[p].first_ord;
	return page_read_stream(&m_live[p], local * len, rec, len);
}

static uint16_t backend_stored(void)
{
	return m_nlive ? (uint16_t)(m_abs_ord - m_live[0].first_ord) : 0;
}

static uint32_t backend_first_abs(void)
{
	return m_nlive ? m_live[0].first_ord : m_abs_ord;
}

static uint16_t backend_nseg(void)
{
	return m_nlive;
}

static void backend_seg(uint16_t i, struct hist_seg *s)
{
	const struct live_page *lp = &m_live[i];

	s->origin = lp->first_ord;
	s->first = lp->first_ord;
	s->end = (i + 1 < m_nlive) ? m_live[i + 1].first_ord : m_abs_ord;
	s->base = lp->base_time;
	s->synced = (lp->flags & LP_SYNCED) != 0;
	s->cur_boot = (lp->flags & LP_CUR_BOOT) != 0;
}

static bool backend_seg_sync(uint16_t i, uint32_t offset)
{
	m_live[i].base_time += offset;
	m_live[i].flags |= LP_SYNCED;
	if (m_live[i].flags & LP_V2) {
		m_live[i].flags |= LP_FIXUP;
		return true; /* a fix-up write is pending */
	}
	return false;
}

/* Program the fix-up double word of every page re-based at a clock sync. Each is
 * programmed at most once (a failed program is not retried: re-programming a
 * non-erased double word fails on the STM32WL anyway); the RAM view already has
 * the unix times, only the persistence across the next reboot is lost. */
static void backend_flush_fixups(void)
{
	for (uint16_t i = 0; i < m_nlive; i++) {
		struct live_page *lp = &m_live[i];

		if (!(lp->flags & LP_FIXUP)) {
			continue;
		}
		lp->flags &= ~LP_FIXUP;

		struct hist_page_hdr h;
		if (read_hdr(lp->phys, &h) != 0 || !hdr_valid(&h) || h.base_synced) {
			continue;
		}
		struct hist_page_fixup f = {
			.offset = lp->base_time - h.base_time,
			.mark = FIXUP_MARK,
			.rsv = 0,
		};
		f.crc = crc16_ccitt(h.crc, (const uint8_t *)&f,
				    offsetof(struct hist_page_fixup, crc));
		int ret = hist_flash_write(page_off(lp->phys) + HIST_HDR_SIZE, &f, sizeof(f));
		if (ret) {
			LOG_WRN("history fix-up write failed: %d", ret);
		}
	}
}

static void backend_reset_logical(void)
{
	/* Drop the live set without erasing. When the reset also changes the
	 * layout/interval, the next page's differing header already breaks
	 * mount-time contiguity with the stale pages, which are then reclaimed as
	 * the ring wraps over them.
	 *
	 * #340 L7: that isn't true for a no-layout-change reset (e.g. `history
	 * sensors <same> on`) -- m_next_seq/m_last_phys are left untouched here, so
	 * the next page written continues the OLD seq numbering exactly one past
	 * the last pre-reset page. backend_mount()'s backward chain walk matches
	 * on `cur.seq - h.seq == 1`, so after a reboot it reattaches that stale
	 * page to the new one, producing an m_abs_ord/m_live[0].first_ord mismatch
	 * that underflows backend_stored(). Burn one seq value so no future page
	 * can ever land exactly 1 above the last pre-reset page's seq.
	 *
	 * m_abs_ord is NOT rewound: absolute ordinals keep growing across a reset
	 * so a replay cursor taken before it lands before the new oldest record and
	 * the replay ends instead of streaming the new layout under the old one. */
	m_nlive = 0;
	m_head_dw = 0;
	m_stage_len = 0;
	m_head_full = false;
	m_next_seq++;
}

static void backend_erase(void)
{
	if (!m_ready) {
		return;
	}
	(void)hist_flash_erase(0, FIXED_PARTITION_SIZE(history_partition));
	backend_reset_logical();
}

static uint16_t backend_capacity(uint16_t sample_size)
{
	uint32_t cap = (uint32_t)HIST_NPAGES * records_per_page(sample_size);
	return (uint16_t)MIN(cap, UINT16_MAX);
}

bool app_history_is_ready(void)
{
	return m_ready;
}

#else /* RAM fallback */

static uint8_t __noinit m_ram[CONFIG_APP_HISTORY_BYTES];
static uint16_t m_ram_start;
static uint16_t m_ram_count;
static uint32_t m_ram_first_abs; /* absolute ordinal of the oldest record (evicted total) */

/* Segment table (32 B): the RAM ring has no pages, so a slot discontinuity
 * (halt, stall, RTC step) starts a new entry here. The ring is cleared on every
 * boot, so all segments are this boot's. When a fifth segment is needed the
 * oldest one is dropped together with its records. */
#define RAM_NSEG 4
struct ram_seg {
	uint32_t origin; /* absolute ordinal of the segment's first record */
	uint32_t base;   /* its time: unix when synced, else uptime-s */
};
static struct ram_seg m_ram_seg[RAM_NSEG]; /* [0] = oldest */
static uint8_t m_ram_nseg;
static uint8_t m_ram_seg_synced; /* bit i: m_ram_seg[i].base is unix time */

static int backend_init(void)
{
	return 0;
}
static bool backend_mount(void)
{
	m_ram_start = 0;
	m_ram_count = 0;
	m_ram_first_abs = 0;
	m_ram_nseg = 0;
	return false; /* RAM ring starts empty each boot */
}
static uint16_t backend_capacity(uint16_t sample_size)
{
	return (uint16_t)(sizeof(m_ram) / sample_size);
}
static void ram_seg_shift(void)
{
	for (uint8_t i = 1; i < m_ram_nseg; i++) {
		m_ram_seg[i - 1] = m_ram_seg[i];
	}
	m_ram_seg_synced >>= 1;
	m_ram_nseg--;
}
static int backend_append(const uint8_t *rec, size_t len, uint32_t base, bool synced, bool split,
			  uint32_t *evicted)
{
	uint16_t cap = backend_capacity(m_sample_size);
	*evicted = 0;

	if (m_ram_count == 0) {
		m_ram_nseg = 0;
	}
	if (m_ram_nseg == 0 || split) {
		if (m_ram_nseg == RAM_NSEG) {
			/* Table full: drop the oldest segment with its records. */
			uint16_t n0 = (uint16_t)(m_ram_seg[1].origin - m_ram_first_abs);

			m_ram_start = (uint16_t)((m_ram_start + n0) % cap);
			m_ram_count -= n0;
			m_ram_first_abs += n0;
			*evicted += n0;
			ram_seg_shift();
		}
		m_ram_seg[m_ram_nseg] = (struct ram_seg){
			.origin = m_ram_first_abs + m_ram_count,
			.base = base,
		};
		WRITE_BIT(m_ram_seg_synced, m_ram_nseg, synced);
		m_ram_nseg++;
	}

	uint16_t slot;
	if (m_ram_count >= cap) {
		slot = m_ram_start;
		m_ram_start = (uint16_t)((m_ram_start + 1) % cap);
		m_ram_first_abs++;
		*evicted += 1;
		/* Drop a segment whose records are all evicted now. */
		while (m_ram_nseg > 1 && m_ram_seg[1].origin <= m_ram_first_abs) {
			ram_seg_shift();
		}
	} else {
		slot = (uint16_t)((m_ram_start + m_ram_count) % cap);
		m_ram_count++;
	}
	memcpy(&m_ram[(size_t)slot * m_sample_size], rec, len);
	return 0;
}
static int backend_read(uint32_t abs, uint8_t *rec, size_t len)
{
	if (abs < m_ram_first_abs || abs - m_ram_first_abs >= m_ram_count) {
		return -EIO;
	}
	uint16_t cap = backend_capacity(m_sample_size);
	uint16_t slot = (uint16_t)((m_ram_start + (abs - m_ram_first_abs)) % cap);
	memcpy(rec, &m_ram[(size_t)slot * m_sample_size], len);
	return 0;
}
static uint16_t backend_stored(void)
{
	return m_ram_count;
}
static uint32_t backend_first_abs(void)
{
	return m_ram_first_abs;
}
static uint16_t backend_nseg(void)
{
	return m_ram_count ? m_ram_nseg : 0;
}
static void backend_seg(uint16_t i, struct hist_seg *s)
{
	s->origin = m_ram_seg[i].origin;
	s->first = (i == 0) ? m_ram_first_abs : m_ram_seg[i].origin;
	s->end = (i + 1 < m_ram_nseg) ? m_ram_seg[i + 1].origin : m_ram_first_abs + m_ram_count;
	s->base = m_ram_seg[i].base;
	s->synced = (m_ram_seg_synced & BIT(i)) != 0;
	s->cur_boot = true;
}
static bool backend_seg_sync(uint16_t i, uint32_t offset)
{
	m_ram_seg[i].base += offset;
	m_ram_seg_synced |= BIT(i);
	return false; /* nothing to persist */
}
static void backend_flush_fixups(void)
{
}
static void backend_reset_logical(void)
{
	/* Absolute ordinals keep growing across a reset (see the flash backend). */
	m_ram_first_abs += m_ram_count;
	m_ram_start = 0;
	m_ram_count = 0;
	m_ram_nseg = 0;
}
static void backend_erase(void)
{
	backend_reset_logical();
}

bool app_history_is_ready(void)
{
	return true; /* RAM backend always available; no flash mount to fail */
}

#endif

/* Time of the record at absolute ordinal `abs` in segment `s`. */
static uint32_t seg_time(const struct hist_seg *s, uint32_t abs)
{
	return s->base + (abs - s->origin) * m_interval;
}

/* Segment holding absolute ordinal `abs` (which must be stored). */
static void seg_of(uint32_t abs, struct hist_seg *s)
{
	uint16_t n = backend_nseg();

	memset(s, 0, sizeof(*s));
	for (uint16_t i = n; i-- > 0;) {
		backend_seg(i, s);
		if (s->first <= abs || i == 0) {
			return;
		}
	}
}

/* ---- Layout / sizing ---------------------------------------------------- */

/* Registry type of a slot: the motherboard, or the configured sensorN_type. */
static uint8_t slot_type(uint8_t slot)
{
	switch (slot) {
	case 0:
		return APP_SENSOR_TYPE_MOTHERBOARD;
	case 1:
		return g_app_config.sensor1_type;
	case 2:
		return g_app_config.sensor2_type;
	case 3:
		return g_app_config.sensor3_type;
	case 4:
		return g_app_config.sensor4_type;
	default:
		return APP_SENSOR_TYPE_NONE;
	}
}

/* The channel an entry names when it can be recorded now, else NULL. */
static const struct app_sensor_channel *entry_channel(uint8_t entry, uint8_t *type)
{
	uint8_t slot = HIST_SLOT(entry);

	if (slot > APP_W1_SLOT_COUNT || (slot > 0 && !g_app_config.cap_w1_sensors)) {
		return NULL;
	}
	*type = slot_type(slot);
	const struct app_sensor_channel *c = app_sensor_channel_get(*type, HIST_CH(entry));

	if (c == NULL || (c->flags & APP_SENSOR_F_RETIRED) || c->hist_enc == APP_SENSOR_HIST_NONE ||
	    c->hist_enc >= ARRAY_SIZE(m_enc_size)) {
		return NULL;
	}
	if (c->cap_off != APP_SENSOR_NO_CAP &&
	    !*(const bool *)((const char *)&g_app_config + c->cap_off)) {
		return NULL;
	}
	return c;
}

/* Rebuild the columns from m_list and the live slot types / capabilities.
 * Returns true when the layout changed. Caller holds m_lock (or is init). */
static bool layout_build(void)
{
	struct hist_col cols[APP_HISTORY_MAX_CH];
	uint8_t n = 0;
	uint16_t size = 0;
	uint32_t crc = 0;

	for (size_t i = 0; i < ARRAY_SIZE(m_list); i++) {
		uint8_t e = m_list[i];
		uint8_t type;
		bool dup = false;

		if (e == APP_HISTORY_ENTRY_UNUSED) {
			continue;
		}
		for (uint8_t k = 0; k < n; k++) {
			dup |= cols[k].entry == e;
		}
		const struct app_sensor_channel *c = dup ? NULL : entry_channel(e, &type);

		if (c == NULL) {
			continue;
		}
		cols[n] = (struct hist_col){
			.desc = c, .entry = e, .type = type, .size = m_enc_size[c->hist_enc]};
		size += cols[n].size;
		/* The guard covers what decoding a stored record depends on. */
		uint8_t key[3] = {e, type, c->hist_enc};

		crc = crc32_ieee_update(crc, key, sizeof(key));
		crc = crc32_ieee_update(crc, (const uint8_t *)&c->hist_scale,
					sizeof(c->hist_scale));
		n++;
	}

	bool changed = n != m_ncols || crc != m_layout || size != m_sample_size;

	memcpy(m_cols, cols, n * sizeof(cols[0]));
	m_ncols = n;
	m_layout = crc;
	m_sample_size = size;
	for (uint8_t s = 0; s < APP_W1_SLOT_COUNT; s++) {
		m_w1_types[s] = slot_type(s + 1);
	}
	if (changed) {
		m_pulses_known = 0;
	}
	/* No channel gives size 0; the RAM backend's capacity is
	 * sizeof(m_ram)/sample_size, so guard the divisor here. capacity 0 disables
	 * the buffer (checked by capture/replay) — "nothing to record". */
	m_capacity = size ? backend_capacity(size) : 0;
	return changed;
}

/* Logical reset only: the record layout changed, so the ring drops its live set
 * and the next append opens a fresh page. Stale pages carry the old layout CRC
 * in their headers, so mount rejects them and the ring reclaims them as it
 * wraps — no full-partition erase stall. (`history clear` still erases.)
 * Caller holds m_lock. */
static void layout_reset(void)
{
	m_count = 0;
	backend_reset_logical();
}

/* ---- Record codec ------------------------------------------------------- */

static void enc_limits(uint8_t enc, int64_t *lo, int64_t *hi)
{
	switch (enc) {
	case APP_SENSOR_HIST_U8:
		*lo = 0, *hi = UINT8_MAX - 1;
		break;
	case APP_SENSOR_HIST_I16:
		*lo = INT16_MIN, *hi = INT16_MAX - 1;
		break;
	case APP_SENSOR_HIST_U16:
		*lo = 0, *hi = UINT16_MAX - 1;
		break;
	case APP_SENSOR_HIST_I32:
		*lo = INT32_MIN, *hi = INT32_MAX - 1;
		break;
	default:
		*lo = 0, *hi = (int64_t)UINT32_MAX - 1;
		break;
	}
}

/* The "absent" sentinel: the top value of the encoding (INT16_MAX / INT32_MAX /
 * all-ones). */
static uint32_t enc_sentinel(uint8_t enc)
{
	int64_t lo, hi;

	enc_limits(enc, &lo, &hi);
	return (uint32_t)(hi + 1);
}

static void put_raw(uint8_t *p, uint8_t size, uint32_t v)
{
	if (size == 1) {
		p[0] = (uint8_t)v;
	} else if (size == 2) {
		sys_put_le16((uint16_t)v, p);
	} else {
		sys_put_le32(v, p);
	}
}

/* Encode column i from g_app_sensor_data (caller holds its lock). */
static size_t encode_value(uint8_t *p, int i)
{
	const struct hist_col *col = &m_cols[i];
	const struct app_sensor_channel *c = col->desc;
	uint8_t slot = HIST_SLOT(col->entry);
	const union app_sensor_value *v;
	uint32_t valid;

	if (slot == 0) {
		v = g_app_sensor_data.mb.v;
		valid = g_app_sensor_data.mb.valid;
	} else {
		const struct app_sensor_w1 *w = &g_app_sensor_data.w1[slot - 1];

		v = w->v;
		/* A slot that does not hold the type the layout was built for (absent,
		 * replaced, re-typed) records nothing. */
		valid = (w->present && w->type == col->type) ? w->valid : 0;
	}

	uint8_t ch = HIST_CH(col->entry);
	uint32_t raw = enc_sentinel(c->hist_enc);
	int64_t x = 0;
	bool have = false;

	if (c->flags & APP_SENSOR_F_MOMENTARY) {
		/* Asserted during the interval = its pulse counter moved since the
		 * previous record; the first record after a (re)start has no base. */
		if (c->pulses_ch != APP_SENSOR_NO_CH && (valid & BIT(c->pulses_ch))) {
			uint32_t cnt = v[c->pulses_ch].u;

			if (m_pulses_known & BIT(i)) {
				x = cnt > m_pulses_prev[i] ? 1 : 0;
				have = true;
			}
			m_pulses_prev[i] = cnt;
			m_pulses_known |= BIT(i);
		}
	} else if (valid & BIT(ch)) {
		if (c->flags & APP_SENSOR_F_COUNTER) {
			x = v[ch].u;
			have = true;
		} else if (!isnan(v[ch].f)) {
			x = llroundf(v[ch].f * c->hist_scale);
			have = true;
		}
	}
	if (have) {
		int64_t lo, hi;

		enc_limits(c->hist_enc, &lo, &hi);
		raw = (uint32_t)CLAMP(x, lo, hi);
	}
	put_raw(p, col->size, raw);
	return col->size;
}

static void decode_record(const uint8_t *rec, struct app_history_record *out)
{
	const uint8_t *p = rec; /* values only — no per-record delta prefix */

	out->present = 0;
	for (int i = 0; i < m_ncols; i++) {
		const struct app_sensor_channel *c = m_cols[i].desc;
		uint32_t raw = m_cols[i].size == 1   ? p[0]
			       : m_cols[i].size == 2 ? sys_get_le16(p)
						     : sys_get_le32(p);

		p += m_cols[i].size;
		out->value[i] = 0;
		if (raw == enc_sentinel(c->hist_enc)) {
			continue;
		}
		double x;

		switch (c->hist_enc) {
		case APP_SENSOR_HIST_I16:
			x = (int16_t)raw;
			break;
		case APP_SENSOR_HIST_I32:
			x = (int32_t)raw;
			break;
		default:
			x = raw;
			break;
		}
		out->value[i] = (c->flags & APP_SENSOR_F_COUNTER) ? x : x / (double)c->hist_scale;
		out->present |= BIT(i);
	}
}

/* ---- Public API --------------------------------------------------------- */

void app_history_set_replay_active(bool active)
{
	m_replay_active = active;
}

/* Deferred clock-sync fix-up writes (flash backend). on_clock_sync() runs in
 * the LoRaWAN downlink callback (system work queue, #96: no flash write there),
 * so it only marks pages and submits this work to the queue registered with
 * app_history_set_work_queue() (app_report's). The next capture flushes too. */
static struct k_work_q *m_maint_q;

static void fixup_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&m_lock, K_FOREVER);
	backend_flush_fixups();
	k_mutex_unlock(&m_lock);
}

static K_WORK_DEFINE(m_fixup_work, fixup_work_handler);

void app_history_set_work_queue(struct k_work_q *queue)
{
	m_maint_q = queue;
}

/* Time the next record has when it continues the newest segment's grid. False
 * when there is none to continue: an empty ring, or a newest segment recorded
 * before this boot (flash) — its grid says nothing about how long the device
 * was off, so the record must open a new segment from the clock (F28). */
static bool head_next_time(uint32_t *t, bool *synced)
{
	uint16_t n = backend_nseg();
	struct hist_seg s;

	if (n == 0) {
		return false;
	}
	backend_seg(n - 1, &s);
	if (!s.cur_boot) {
		return false;
	}
	*t = seg_time(&s, s.end);
	*synced = s.synced;
	return true;
}

/* Does a record for `slot` continue the head segment whose next record is due
 * at `next`? Same clock domain and within half an interval: a timer that fired
 * a little early or late, a small RTC correction, or the second or so between
 * app_report's and history's uptime -> unix conversion. Anything else — missed
 * slots (MCU halted or stalled, a dropped record), an RTC step — must not be
 * folded into the grid. */
static bool slot_continues(uint32_t next, bool next_synced, uint32_t slot, bool slot_synced)
{
	if (next_synced != slot_synced) {
		return false;
	}
	int32_t d = (int32_t)(slot - next);
	int32_t half = (int32_t)(m_interval / 2);

	return d >= -half && d <= half;
}

static void capture(bool have_slot, uint32_t slot, bool slot_synced)
{
	/* An empty layout is re-checked below: a slot typed later may fill it. */
	if (!m_enabled || !app_history_is_ready()) {
		return;
	}

	uint8_t rec[MAX_RECORD_SIZE];

	k_mutex_lock(&m_lock, K_FOREVER);

	/* Clock-sync fix-ups not yet written (no work queue registered, or its work
	 * still queued): this runs on the report work queue, a safe context. */
	backend_flush_fixups();

	/* Records are periodic at interval_report, so per-record time is implicit
	 * (base + ord*interval). If the interval changed, that timebase no longer
	 * holds — drop the history and restart at the new rate. Logical reset only:
	 * no full erase here (that stalls the CPU on the TX path); the ring drops
	 * the live set and the next append opens a fresh page whose header breaks
	 * mount-time continuity with the stale pages (#96, #265). */
	if (m_interval != (uint32_t)g_app_config.interval_report) {
		m_interval = (uint32_t)g_app_config.interval_report;
		backend_reset_logical();
		m_count = 0;
	}

	/* A slot re-typed / a capability toggled since the last record changes the
	 * columns: restart the buffer on the new layout (#430). */
	if (layout_build()) {
		LOG_INF("history layout changed: %u channels, %u B/record", m_ncols, m_sample_size);
		layout_reset();
	}
	if (m_sample_size == 0 || m_capacity == 0) {
		k_mutex_unlock(&m_lock);
		return;
	}

	/* Snapshot sensor values atomically w.r.t. the sensor data. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	size_t pos = 0;
	for (int i = 0; i < m_ncols; i++) {
		pos += encode_value(&rec[pos], i);
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* The base a new segment (flash: page) gets if this record opens one, and
	 * whether it must open one. On the report cadence that is the record's
	 * slot (RTC time), and a slot that doesn't continue this boot's grid splits
	 * the ring. Off the cadence (`history capture`) it is the continuation of
	 * this boot's grid, else the clock right now (RTC when set, else uptime). */
	uint32_t next;
	bool next_synced;
	bool cont = head_next_time(&next, &next_synced);
	uint32_t base;
	bool synced;
	bool split = false;

	if (have_slot) {
		base = slot;
		synced = slot_synced;
		if (cont && !slot_continues(next, next_synced, slot, slot_synced)) {
			LOG_WRN("history slot %u off the grid (next %u, %s): new segment", slot,
				next, slot_synced == next_synced ? "same clock" : "clock changed");
			split = true;
		}
	} else if (cont) {
		base = next;
		synced = next_synced;
	} else {
		base = now_seconds(&synced);
	}

	/* Only advance the logical view if the write lands, so a failed flash write
	 * leaves no phantom record (#96). */
	uint32_t evicted = 0;
	int ret = backend_append(rec, m_sample_size, base, synced, split, &evicted);
	if (ret == -EBUSY) {
		LOG_WRN("history page rollover held off by a replay, record dropped");
	} else if (ret != 0) {
		LOG_WRN("history append failed — record dropped");
	}
	/* #340 L8: advance_page() may have committed a real eviction before a
	 * per-record write failed, so refresh the count on every path. */
	m_count = backend_stored();

	k_mutex_unlock(&m_lock);
}

void app_history_capture(void)
{
	capture(false, 0, false);
}

void app_history_capture_at(uint32_t slot, bool synced)
{
	capture(true, slot, synced);
}

void app_history_on_clock_sync(uint32_t unix_now)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	/* Segments stamped on this boot's uptime (no RTC yet) become unix time by
	 * the offset between the two clocks now. Segments from an earlier boot keep
	 * base_synced=0 unless their fix-up double word was written: their uptime
	 * epoch ended with that boot and cannot be recovered (#191 used to guess
	 * "newest record = now"; a wrong absolute time is worse than an honest
	 * unsynced frame). Already-synced segments keep their times; an RTC step
	 * shows up as a slot discontinuity at the next capture instead.
	 *
	 * No flash write here (#96): this runs inside the LoRaWAN downlink callback
	 * (LoRaMacProcess on the system workqueue). The flash backend persists the
	 * offset in each re-based page's fix-up double word from the report work
	 * queue instead, so the unix times survive a later reboot. */
	uint32_t off = unix_now - (uint32_t)(k_uptime_get() / 1000);
	uint16_t n = backend_nseg();
	bool pending = false;

	for (uint16_t i = 0; i < n; i++) {
		struct hist_seg s;

		backend_seg(i, &s);
		if (!s.synced && s.cur_boot) {
			pending |= backend_seg_sync(i, off);
		}
	}
	k_mutex_unlock(&m_lock);

	if (pending && m_maint_q) {
		(void)k_work_submit_to_queue(m_maint_q, &m_fixup_work);
	}
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
	m_count = 0;
	k_mutex_unlock(&m_lock);
}

int app_history_get(size_t idx, struct app_history_record *out)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	if (idx >= m_count) {
		k_mutex_unlock(&m_lock);
		return -ENOENT;
	}

	/* Records are periodic within their segment: time = base + ord*interval. */
	uint32_t abs = backend_first_abs() + (uint32_t)idx;
	struct hist_seg s;
	seg_of(abs, &s);
	out->time_unix = seg_time(&s, abs);
	out->time_synced = s.synced;

	uint8_t rec[MAX_RECORD_SIZE];
	if (backend_read(abs, rec, m_sample_size) != 0) {
		/* Don't decode an uninitialised buffer (#96) — report all-absent. */
		out->present = 0;
		k_mutex_unlock(&m_lock);
		return -EIO;
	}
	decode_record(rec, out);

	k_mutex_unlock(&m_lock);
	return 0;
}

uint32_t app_history_get_interval(void)
{
	return m_interval;
}

/* Whether records of an unsynced segment belong in the [from, to] window. Their
 * time is uptime (or an ended boot's uptime), so a unix window cannot be applied:
 * they are returned for an open window (from 0 to UINT32_MAX, what a host sends
 * without bounds) and while the device itself has no wall clock (as before: the
 * whole buffer until the clock is synced), never for a bounded window on a
 * synced device — a Portal gap fill must not pull stale unsynced pages each
 * time. The frame carries time_synced=false either way. */
static bool window_takes_unsynced(uint32_t from_unix, uint32_t to_unix)
{
	bool rtc;

	(void)now_seconds(&rtc);
	return !rtc || (from_unix == 0 && to_unix == UINT32_MAX);
}

/* Records of segment `s` inside the window: absolute ordinals [*lo, *hi). Times
 * are monotonic within a segment, so this is one contiguous run. */
static void seg_window(const struct hist_seg *s, uint32_t from_unix, uint32_t to_unix,
		       bool take_unsynced, uint32_t *lo, uint32_t *hi)
{
	uint64_t a = s->first;
	uint64_t b = s->end;

	if (!s->synced) {
		if (!take_unsynced) {
			b = a;
		}
	} else if (m_interval == 0) {
		if (s->base < from_unix || s->base > to_unix) {
			b = a;
		}
	} else {
		/* time(abs) = base + (abs - origin) * interval; a synced base is a
		 * plausible unix time (>= 2024), so these differences fit uint32. */
		if (from_unix > s->base) {
			uint32_t d = from_unix - s->base;
			uint32_t k = d / m_interval + ((d % m_interval) ? 1 : 0);

			a = MAX(a, (uint64_t)s->origin + k);
		}
		if (to_unix < s->base) {
			b = a;
		} else {
			uint32_t k = (to_unix - s->base) / m_interval;

			b = MIN(b, (uint64_t)s->origin + k + 1);
		}
	}
	if (a > b) {
		a = b;
	}
	*lo = (uint32_t)a;
	*hi = (uint32_t)b;
}

/* First record at or after `abs` (and before `end`) that lies in the window, or
 * `end` (a value >= end) when none is left. */
static uint32_t next_in_window(uint32_t abs, uint32_t end, uint32_t from_unix, uint32_t to_unix,
			       bool take_unsynced)
{
	uint16_t nseg = backend_nseg();

	for (uint16_t i = 0; i < nseg && abs < end; i++) {
		struct hist_seg s;
		uint32_t lo, hi;

		backend_seg(i, &s);
		if (abs >= s.end) {
			continue;
		}
		seg_window(&s, from_unix, to_unix, take_unsynced, &lo, &hi);
		if (abs < lo) {
			abs = lo;
		}
		if (abs < hi) {
			return MIN(abs, end);
		}
		abs = s.end;
	}
	return MAX(abs, end);
}

/* Records have a fixed size (m_sample_size, values only) and a shared present
 * column layout (m_cols) — so a wire frame carries the layout + interval once and each
 * record is just the raw stored bytes (sentinels mark absent values). Time is
 * implicit and periodic within a segment, so a frame never crosses a segment
 * boundary: its t0 and time_synced are the segment's, and the host's
 * t0 + j * interval_s holds.
 *
 * Cursors here are absolute ordinals (backend_first_abs() + ord): a record keeps
 * its absolute ordinal while newer records are appended and older ones evicted,
 * so a replay that runs across captures neither repeats nor skips records. A
 * cursor that has fallen out of the ring (its record evicted) resumes at the
 * oldest record still stored. `end` bounds the scan (exclusive).
 *
 * *next_out is the next record IN THE WINDOW (records past to_unix or outside
 * it are skipped), or >= end when none is left — so a caller knows after the
 * last frame that the window is exhausted, without another empty frame (H-4). */
static size_t export_locked(uint32_t from_unix, uint32_t to_unix, uint32_t start, uint32_t end,
			    uint8_t *buf, size_t cap, uint32_t *t0_out, bool *synced_out,
			    uint16_t *n_written, uint32_t *next_out)
{
	uint32_t first = backend_first_abs();
	uint32_t stop = first + m_count;
	bool take_unsynced = window_takes_unsynced(from_unix, to_unix);
	size_t pos = 0;
	uint16_t written = 0;
	uint32_t t0 = 0;
	bool synced = false;

	if (end > stop) {
		end = stop;
	}
	if (start < first) {
		start = first; /* evicted under the cursor: resume at the oldest record */
	}

	uint32_t abs = start;
	uint16_t nseg = backend_nseg();

	for (uint16_t i = 0; i < nseg && abs < end; i++) {
		struct hist_seg s;
		uint32_t lo, hi;
		bool closed = false;

		backend_seg(i, &s);
		if (abs >= s.end) {
			continue;
		}
		seg_window(&s, from_unix, to_unix, take_unsynced, &lo, &hi);
		if (hi > end) {
			hi = end;
		}
		if (abs < lo) {
			abs = lo;
		}
		for (; abs < hi; abs++) {
			if (pos + m_sample_size > cap) {
				closed = true; /* this record spills to the next frame */
				break;
			}
			if (backend_read(abs, buf + pos, m_sample_size) != 0) {
				/* M-13: a single unreadable record (flash read error on a
				 * corrupt page) must not truncate the whole replay. Skip it
				 * instead of ending the frame — but never leave a gap *inside*
				 * a frame, since the host reconstructs times as t0 +
				 * j*interval (contiguous). If we already packed records, close
				 * this frame and resume AFTER the bad record; otherwise skip it
				 * and keep looking for the frame's first good record.
				 * (pos/written are not advanced, so the uninitialised bytes are
				 * overwritten by the next good read — #96 still holds.) */
				LOG_WRN("history record %u read failed — skipping (M-13)", abs);
				if (written > 0) {
					abs++;
					closed = true;
					break;
				}
				continue;
			}
			if (written == 0) {
				t0 = seg_time(&s, abs);
				synced = s.synced;
			}
			pos += m_sample_size;
			written++;
		}
		if (closed || written > 0) {
			break; /* a frame never crosses a segment boundary */
		}
		/* Nothing of this segment in the window: go on with the next one. */
		abs = MIN(s.end, end);
	}

	if (t0_out) {
		*t0_out = t0;
	}
	if (synced_out) {
		*synced_out = synced;
	}
	if (n_written) {
		*n_written = written;
	}
	if (next_out) {
		*next_out = next_in_window(abs, end, from_unix, to_unix, take_unsynced);
	}
	return pos;
}

size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, bool *synced_out, uint16_t *n_written,
			       size_t *next_ord)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	uint32_t first = backend_first_abs();
	uint32_t start = (start_ord < m_count) ? first + (uint32_t)start_ord : first + m_count;
	uint32_t next = start;
	size_t pos = export_locked(from_unix, to_unix, start, first + m_count, buf, cap, t0_out,
				   synced_out, n_written, &next);

	if (next_ord) {
		/* Past-the-end cursors are echoed back unchanged (has_more=false). */
		*next_ord = (start_ord < m_count) ? (size_t)(next - first) : start_ord;
	}

	k_mutex_unlock(&m_lock);
	return pos;
}

void app_history_span(uint32_t *first_abs, uint32_t *end_abs)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint32_t first = backend_first_abs();

	if (first_abs) {
		*first_abs = first;
	}
	if (end_abs) {
		*end_abs = first + m_count;
	}
	k_mutex_unlock(&m_lock);
}

size_t app_history_export_abs(uint32_t from_unix, uint32_t to_unix, uint32_t start_abs,
			      uint32_t end_abs, uint8_t *buf, size_t cap, uint32_t *t0_out,
			      bool *synced_out, uint16_t *n_written, uint32_t *next_abs)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	size_t pos = export_locked(from_unix, to_unix, start_abs, end_abs, buf, cap, t0_out,
				   synced_out, n_written, next_abs);
	k_mutex_unlock(&m_lock);
	return pos;
}

/* Number of frames the [from,to] window needs at `cap` bytes/frame. Mirrors the
 * packing in export_page (fixed record size → whole records per frame, a frame
 * never crosses a segment boundary). */
uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	if (m_sample_size == 0 || cap < m_sample_size) {
		return 0;
	}
	uint32_t per_frame = (uint32_t)(cap / m_sample_size);

	k_mutex_lock(&m_lock, K_FOREVER);
	bool take_unsynced = window_takes_unsynced(from_unix, to_unix);
	uint16_t nseg = backend_nseg();
	uint32_t frames = 0;

	for (uint16_t i = 0; i < nseg; i++) {
		struct hist_seg s;
		uint32_t lo, hi;

		backend_seg(i, &s);
		seg_window(&s, from_unix, to_unix, take_unsynced, &lo, &hi);
		frames += (hi - lo + per_frame - 1) / per_frame;
	}
	k_mutex_unlock(&m_lock);

	return (uint16_t)MIN(frames, UINT16_MAX);
}

void app_history_set_enabled(bool enable)
{
	m_enabled = enable;
}

/* Record size is the sum of the selected columns' encodings (1..4 B each):
 * the factory default (temperature + humidity) is 3 B/record, the worst case
 * (24 four-byte columns) 96 B. This directly bounds records-per-tap on the NFC
 * paged readout (req_history_page), whose samples payload is capped well under
 * 512 B (see DUMP_PAGE_BUDGET_NFC in app_cmd.c) — selecting more channels
 * trades off history depth per NFC tap. */

void app_history_get_layout(struct app_history_layout *out)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	memset(out, 0, sizeof(*out));
	out->count = m_ncols;
	for (uint8_t i = 0; i < m_ncols; i++) {
		out->channels[i] = m_cols[i].entry;
		out->has_w1 |= HIST_SLOT(m_cols[i].entry) != 0;
	}
	memcpy(out->w1_types, m_w1_types, sizeof(out->w1_types));
	k_mutex_unlock(&m_lock);
}

void app_history_set_channels(const uint8_t *list, size_t n)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	memset(m_list, APP_HISTORY_ENTRY_UNUSED, sizeof(m_list));
	memcpy(m_list, list, MIN(n, sizeof(m_list)));
	if (layout_build()) {
		layout_reset();
	}
	k_mutex_unlock(&m_lock);
}

void app_history_get_channels(uint8_t list[APP_HISTORY_MAX_CH])
{
	k_mutex_lock(&m_lock, K_FOREVER);
	memcpy(list, m_list, sizeof(m_list));
	k_mutex_unlock(&m_lock);
}

int app_history_entry_name(uint8_t entry, char *buf, size_t cap)
{
	uint8_t slot = HIST_SLOT(entry);
	const struct app_sensor_channel *c =
		slot <= APP_W1_SLOT_COUNT ? app_sensor_channel_get(slot_type(slot), HIST_CH(entry))
					  : NULL;

	if (c == NULL) {
		snprintf(buf, cap, "?%02x", entry);
		return -ENOENT;
	}
	if (slot == 0) {
		snprintf(buf, cap, "%s", c->name);
	} else {
		snprintf(buf, cap, "s%u-%s", slot, c->name);
	}
	return 0;
}

int app_history_entry_by_name(const char *name, uint8_t *entry)
{
	uint8_t slot = 0;

	/* "s<N>-<channel>" names a channel of 1-Wire slot N's configured type. */
	if (name[0] == 's' && name[1] >= '1' && name[1] <= '0' + APP_W1_SLOT_COUNT &&
	    name[2] == '-') {
		slot = (uint8_t)(name[1] - '0');
		name += 3;
	}
	int ch = app_sensor_channel_by_name(slot_type(slot), name);

	if (ch < 0) {
		return -EINVAL;
	}
	*entry = (uint8_t)(slot << 5 | ch);
	return 0;
}

bool app_history_entry_available(uint8_t entry)
{
	uint8_t type;

	return entry_channel(entry, &type) != NULL;
}

int app_history_init(void)
{
	k_mutex_init(&m_lock);

	m_enabled = g_app_config.history_enable;
	memcpy(m_list, g_app_config.history_channels, sizeof(m_list));

	int ret = backend_init();
	if (ret) {
		LOG_WRN("history backend init failed: %d (history unavailable)", ret);
		m_capacity = 0;
		return ret;
	}

	(void)layout_build();

	/* Restore a prior ring if its layout matches; else start clean. On success
	 * backend_mount() sets m_interval and each page's time base from the page
	 * headers; m_interval = 0 otherwise forces the first capture to seed it from
	 * interval_report. */
	if (backend_mount()) {
		m_count = backend_stored();
	} else {
		m_count = 0;
		m_interval = 0;
	}

	LOG_INF("history: enabled=%d, %u channels, sample=%uB, capacity=%u, stored=%u", m_enabled,
		m_ncols, m_sample_size, m_capacity, m_count);
	return 0;
}

/* ---- Shell -------------------------------------------------------------- */

#if defined(CONFIG_SHELL)

static const char *backend_name(void)
{
	return IS_ENABLED(CONFIG_APP_HISTORY_FLASH) ? "flash" : "ram";
}

static void print_value(char *buf, size_t cap, int i, bool present, double v)
{
	const struct app_sensor_channel *c = m_cols[i].desc;

	if (!present) {
		snprintf(buf, cap, "--");
	} else if ((c->flags & APP_SENSOR_F_COUNTER) || c->hist_scale <= 1.0f) {
		/* Counters, states and enums (orientation) are whole numbers. */
		snprintf(buf, cap, "%d", APP_FP0(v));
	} else {
		snprintf(buf, cap, "%s%d.%02d", APP_FP2(v));
	}
}

static void col_name(int i, char *buf, size_t cap)
{
	(void)app_history_entry_name(m_cols[i].entry, buf, cap);
}

static int cmd_history_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "enabled:   %s", m_enabled ? "yes" : "no");
	shell_print(sh, "backend:   %s", backend_name());

	char list[256] = "";
	for (int i = 0; i < m_ncols; i++) {
		char name[32];

		col_name(i, name, sizeof(name));
		if (list[0]) {
			strncat(list, ", ", sizeof(list) - strlen(list) - 1);
		}
		strncat(list, name, sizeof(list) - strlen(list) - 1);
	}
	shell_print(sh, "channels:  %s (%u ch, %u B/record)", list[0] ? list : "(none)", m_ncols,
		    m_sample_size);
	shell_print(sh, "capacity:  %u records", m_capacity);

	k_mutex_lock(&m_lock, K_FOREVER);
	uint16_t count = m_count;
	uint16_t nseg = backend_nseg();
	struct hist_seg s = {0};
	if (nseg > 0) {
		backend_seg(0, &s);
	}
	k_mutex_unlock(&m_lock);

	shell_print(sh, "stored:    %u / %u", count, m_capacity);
	/* Oldest record's time; each segment (flash page) has its own base. */
	shell_print(sh, "base:      %u (%s)", nseg ? seg_time(&s, s.first) : 0,
		    s.synced ? "unix" : "uptime/no-rtc");
	shell_print(sh, "segments:  %u", nseg);
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
	char hdr[256];
	int off = snprintf(hdr, sizeof(hdr), "%-5s %-20s", "#", "time");
	for (int i = 0; i < m_ncols && off < (int)sizeof(hdr); i++) {
		char name[32];

		col_name(i, name, sizeof(name));
		off += snprintf(hdr + off, sizeof(hdr) - off, " %8s", name);
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
			/* #340 L9: use the time snapshotted under m_lock at fetch time, never
			 * a live base. Unsynced = uptime seconds of the boot that recorded
			 * the record's segment. */
			snprintf(tbuf, sizeof(tbuf), "up %us (no-rtc)", (unsigned)r.time_unix);
		}

		char line[256];
		int lo = snprintf(line, sizeof(line), "%-5u %-20s", (unsigned)k, tbuf);
		for (int i = 0; i < m_ncols && lo < (int)sizeof(line); i++) {
			char vb[16];
			print_value(vb, sizeof(vb), i, r.present & BIT(i), r.value[i]);
			lo += snprintf(line + lo, sizeof(line) - lo, " %8s", vb);
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

/* Sample the sensors and store one record now — normally a capture happens on
 * the periodic report path (needs a network join); this lets a bench/ATS test
 * exercise the buffer without one. */
static int cmd_history_capture(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!m_enabled) {
		shell_warn(sh, "history is disabled (history enable on)");
		return 0;
	}
	app_sensor_sample();
	app_history_capture();
	shell_print(sh, "captured; %u record(s) stored", (unsigned)app_history_count());
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
	uint8_t list[APP_HISTORY_MAX_CH];

	app_history_get_channels(list);

	if (argc == 1) {
		/* Every channel of the motherboard and of each typed 1-Wire slot. */
		shell_print(sh, "%-24s %-7s %s", "channel", "stored", "capability");
		for (uint8_t slot = 0; slot <= APP_W1_SLOT_COUNT; slot++) {
			const struct app_sensor_type *t = app_sensor_type_get(slot_type(slot));

			for (uint8_t ch = 0; t != NULL && ch < t->channel_count; ch++) {
				uint8_t e = (uint8_t)(slot << 5 | ch);
				char name[32];
				bool stored = false;

				if (t->channels[ch].flags & APP_SENSOR_F_RETIRED) {
					continue;
				}
				for (int k = 0; k < m_ncols; k++) {
					stored |= m_cols[k].entry == e;
				}
				(void)app_history_entry_name(e, name, sizeof(name));
				shell_print(sh, "%-24s %-7s %s", name, stored ? "yes" : "no",
					    app_history_entry_available(e) ? "available" : "off");
			}
		}
		return 0;
	}

	if (argc != 3) {
		shell_error(sh, "usage: history sensors [<channel> on|off]");
		return -EINVAL;
	}

	uint8_t e;

	if (app_history_entry_by_name(argv[1], &e) != 0) {
		shell_error(sh, "unknown channel `%s`", argv[1]);
		return -EINVAL;
	}
	bool on = strcmp(argv[2], "on") == 0;
	if (!on && strcmp(argv[2], "off") != 0) {
		shell_error(sh, "usage: history sensors <channel> on|off");
		return -EINVAL;
	}
	if (on && !app_history_entry_available(e)) {
		shell_error(sh, "`%s` capability is off", argv[1]);
		return -EINVAL;
	}

	/* Drop the entry, then append it when on: the list stays compact. */
	uint8_t out[APP_HISTORY_MAX_CH];
	size_t n = 0;

	memset(out, APP_HISTORY_ENTRY_UNUSED, sizeof(out));
	for (size_t k = 0; k < ARRAY_SIZE(list); k++) {
		if (list[k] != APP_HISTORY_ENTRY_UNUSED && list[k] != e) {
			out[n++] = list[k];
		}
	}
	if (on) {
		if (n == ARRAY_SIZE(out)) {
			shell_error(sh, "at most %u channels", APP_HISTORY_MAX_CH);
			return -ENOSPC;
		}
		out[n++] = e;
	}
	app_history_set_channels(out, sizeof(out));
	memcpy(app_config()->history_channels, out, sizeof(out)); /* staged */
	shell_print(sh, "channel `%s` %s; buffer cleared", argv[1], on ? "enabled" : "disabled");
	return 0;
}

static int cmd_history_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	size_t count = app_history_count();
	shell_print(sh, "%-20s %8s %8s %8s %5s", "channel", "min", "max", "avg", "n");

	for (int i = 0; i < m_ncols; i++) {
		char name[32];
		double mn = 0, mx = 0, sum = 0;
		unsigned n = 0;

		col_name(i, name, sizeof(name));
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
			shell_print(sh, "%-20s %8s %8s %8s %5u", name, "--", "--", "--", 0);
		} else {
			shell_print(sh, "%-20s %s%d.%02d %s%d.%02d %s%d.%02d %5u", name,
				    APP_FP2(mn), APP_FP2(mx), APP_FP2(sum / n), n);
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_history, SHELL_CMD_ARG(info, NULL, "Buffer summary.", cmd_history_info, 1, 0),
	SHELL_CMD_ARG(count, NULL, "Number of stored records.", cmd_history_count, 1, 0),
	SHELL_CMD_ARG(read, NULL, "List records. Usage: read [N]", cmd_history_read, 1, 1),
	SHELL_CMD_ARG(clear, NULL, "Erase the buffer.", cmd_history_clear, 1, 0),
	SHELL_CMD_ARG(capture, NULL, "Sample sensors and store one record now (test).",
		      cmd_history_capture, 1, 0),
	SHELL_CMD_ARG(sensors, NULL, "List/select channels. Usage: sensors [<channel> on|off]",
		      cmd_history_sensors, 1, 2),
	SHELL_CMD_ARG(enable, NULL, "Master on/off. Usage: enable on|off", cmd_history_enable, 2,
		      0),
	SHELL_CMD_ARG(stats, NULL, "Per-channel min/max/avg.", cmd_history_stats, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(history, &sub_history, "Sensor history store-and-forward.", NULL);

#endif /* defined(CONFIG_SHELL) */
