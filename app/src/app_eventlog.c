/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_eventlog.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio.h"

#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_EVENTLOG_HAVE_CLOCK 1
#endif

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_eventlog, LOG_LEVEL_DBG);

/* Flash page ring at the top of history_partition (log_split % of it; history
 * keeps the bottom). Every page opens with a 16 B header and a fix-up double
 * word, followed by 126 fixed 16 B records. Records hold their own time: unix
 * seconds on a page opened with the clock synced, else this boot's uptime
 * seconds, turned into unix by the page's fix-up offset (programmed once at
 * the clock sync, like history's). A page from an earlier boot without a
 * fix-up keeps unsynced (uptime) times. Every boot opens a new page.
 *
 * What was uploaded is remembered by SENT records (rule 0xFD, value = the
 * absolute ordinal of the first record not uploaded yet), so unsent events
 * survive a reboot. Ordinal = page seq * EV_RPP + record index. */
#define EV_PAGE_SIZE   2048
#define EV_TOTAL_PAGES (FIXED_PARTITION_SIZE(history_partition) / EV_PAGE_SIZE)
#define EV_MAGIC       0x45564c31 /* "EVL1" */
#define EV_HDR_SIZE    16
#define EV_FIXUP_OFF   EV_HDR_SIZE
#define EV_DATA_OFF    (EV_HDR_SIZE + 8)
#define EV_REC_SIZE    16
#define EV_RPP         ((EV_PAGE_SIZE - EV_DATA_OFF) / EV_REC_SIZE) /* 126 */
#define EV_REC_MARK    0xA5
#define EV_FIXUP_MARK  0x5A
#define EV_RULE_SENT   0xFD

/* Events uploaded per batch (one AlarmReport sequence of pages). */
#define EVENTLOG_CHUNK          64
/* Room of one AlarmReport frame (APP_RADIO_TX_SLOT_SIZE). */
#define EVENTLOG_FRAME_MAX      APP_RADIO_TX_SLOT_SIZE
/* AlarmReport.events max_count (app_config.options.in). */
#define EVENTLOG_PAGE_EVENTS    8
/* Worst-case (one-byte varint) page numbers for the layout pass. */
#define EVENTLOG_PAGE_BOUND     127
/* Answer slots left free for command responses while a batch streams. */
#define EVENTLOG_ANSWER_RESERVE 2
/* A page that found no free slot is tried again after this. */
#define EVENTLOG_RETRY_MS       5000
/* Poll cadence while neither trigger is armed (picks up a config change). */
#define EVENTLOG_IDLE_POLL_S    60
/* Edges waiting for the flash writer (alarm context never writes flash). */
#define EVENTLOG_PENDING        16

struct ev_hdr {
	uint32_t magic;
	uint32_t seq;
	uint8_t synced; /* 1: records carry unix time */
	uint8_t split;  /* log_split the ring was laid out for */
	uint8_t rsv[4];
	uint16_t crc; /* crc16-ccitt over the preceding 14 bytes */
} __packed;
BUILD_ASSERT(sizeof(struct ev_hdr) == EV_HDR_SIZE, "event page header must be 16 B");

struct ev_fixup {
	uint32_t offset; /* unix - uptime (s) */
	uint8_t mark;
	uint8_t rsv;
	uint16_t crc;
} __packed;
BUILD_ASSERT(sizeof(struct ev_fixup) == 8, "fix-up must be one double word");

struct ev_rec {
	uint32_t time; /* unix (synced page) or uptime seconds */
	int32_t value;
	uint8_t rule;
	uint8_t slot;
	uint8_t channel;
	uint8_t edge;
	uint8_t type;
	uint8_t sensor_type;
	uint8_t has_value;
	uint8_t mark; /* EV_REC_MARK; 0xFF = unwritten */
} __packed;
BUILD_ASSERT(sizeof(struct ev_rec) == EV_REC_SIZE, "event record must be 16 B");

struct ev_page {
	uint32_t seq;
	uint32_t offset; /* fix-up offset (valid with EVP_FIXED) */
	uint16_t phys;
	uint8_t nrec; /* written records */
	uint8_t flags;
};
#define EVP_SYNCED   BIT(0) /* records carry unix time */
#define EVP_FIXED    BIT(1) /* fix-up offset known (flash or this boot's clock) */
#define EVP_CUR_BOOT BIT(2) /* opened during this boot */
#define EVP_FIXUP_WR BIT(3) /* fix-up double word still to be programmed */

static K_MUTEX_DEFINE(m_lock);

static const struct flash_area *m_fa;
static off_t m_base; /* offset of the ring in the partition */
static uint16_t m_npages;
static bool m_ready;

static struct ev_page m_pages[EV_TOTAL_PAGES]; /* [0] = oldest .. [n-1] = head */
static uint16_t m_nlive;
static uint32_t m_next_seq;
static uint16_t m_last_phys;
static bool m_head_open; /* the head page was opened during this boot */

static uint32_t m_sent_abs; /* first ordinal not uploaded yet */
static uint32_t m_lost;     /* unsent events overwritten by the ring */
static uint16_t m_unsent;   /* events at/after m_sent_abs */
static uint16_t m_new;      /* edges since the last flush (count trigger) */
static bool m_force;
static int64_t m_last_flush_ms;

/* Edges queued by app_eventlog_add() for the writer work. */
static struct {
	struct app_cmd_alarm_event ev;
	int64_t at_ms;
} m_pend[EVENTLOG_PENDING];
static uint8_t m_pend_n;

/* Upload in progress: a chunk of selected records, paged up front. */
static struct app_cmd_alarm_event m_tx[EVENTLOG_CHUNK];
static uint8_t m_tx_page_n[EVENTLOG_CHUNK];
static uint16_t m_tx_total;
static uint16_t m_tx_first;
static uint8_t m_tx_pages;
static uint8_t m_tx_page;
static uint32_t m_tx_base;
static bool m_tx_synced;

/* Scan cursor of the running upload. */
static bool m_up_active;
static bool m_up_replay;
static uint32_t m_up_from, m_up_to;
static uint32_t m_up_next_abs; /* next ordinal to look at */
static uint32_t m_up_end_abs;  /* stop before this ordinal */
static bool m_req;
static uint32_t m_req_from, m_req_to;

static struct k_work_delayable m_flush_work;
static struct k_work m_write_work;
/* Own queue: flash erase/program may wait for the radio (app_radio_flash_hold). */
static K_THREAD_STACK_DEFINE(m_q_stack, 1536);
static struct k_work_q m_q_obj;
static struct k_work_q *const m_q = &m_q_obj;

/* ---- split ---------------------------------------------------------------- */

static uint8_t split_pct(void)
{
	int s = g_app_config.log_split;

	return (uint8_t)(CLAMP(s, 0, 100) / 25 * 25);
}

uint16_t app_eventlog_flash_pages(void)
{
	/* Debug/provisioning images overlap history_partition: never touch it */
	if (!IS_ENABLED(CONFIG_APP_HISTORY_FLASH)) {
		return 0;
	}
	return (uint16_t)(EV_TOTAL_PAGES * split_pct() / 100);
}

/* The split the flash is laid out for, kept in settings so a change erases
 * the whole partition (history and event log) on the next boot. */
static int m_saved_split = -1; /* -1: never stored (all history) */

static int split_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t v;

	if (strcmp(key, "split") == 0 && len == sizeof(v) && read_cb(cb_arg, &v, sizeof(v)) > 0) {
		m_saved_split = v;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_evlog, "evlog", NULL, split_set, NULL, NULL);

/* ---- flash helpers -------------------------------------------------------- */

static off_t page_off(uint16_t phys)
{
	return m_base + (off_t)phys * EV_PAGE_SIZE;
}

static int ev_write(off_t off, const void *data, size_t len)
{
	app_radio_flash_hold();
	int ret = flash_area_write(m_fa, off, data, len);
	app_radio_flash_release();
	return ret;
}

static int ev_erase(off_t off, size_t len)
{
	app_radio_flash_hold();
	int ret = flash_area_erase(m_fa, off, len);
	app_radio_flash_release();
	return ret;
}

static void hdr_crc_set(struct ev_hdr *h)
{
	h->crc = crc16_ccitt(0xffff, (const uint8_t *)h, offsetof(struct ev_hdr, crc));
}

static bool hdr_valid(const struct ev_hdr *h)
{
	return h->magic == EV_MAGIC &&
	       h->crc == crc16_ccitt(0xffff, (const uint8_t *)h, offsetof(struct ev_hdr, crc));
}

static uint32_t page_abs(const struct ev_page *p, uint8_t idx)
{
	return p->seq * EV_RPP + idx;
}

static bool now_unix(uint32_t *out)
{
#ifdef APP_EVENTLOG_HAVE_CLOCK
	return app_clock_get_unix(out) == 0;
#else
	ARG_UNUSED(out);
	return false;
#endif
}

static uint32_t now_up_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

/* ---- mount ---------------------------------------------------------------- */

static int read_rec(const struct ev_page *p, uint8_t idx, struct ev_rec *r)
{
	return flash_area_read(m_fa, page_off(p->phys) + EV_DATA_OFF + idx * EV_REC_SIZE, r,
			       sizeof(*r));
}

static bool rec_erased(const struct ev_rec *r)
{
	const uint8_t *b = (const uint8_t *)r;

	for (size_t i = 0; i < sizeof(*r); i++) {
		if (b[i] != 0xFF) {
			return false;
		}
	}
	return true;
}

/* Records in a page: the first fully erased slot ends it (a torn record —
 * not erased, bad mark — is skipped on read but still counts). */
static uint8_t scan_nrec(const struct ev_page *p)
{
	struct ev_rec r;
	uint8_t n = 0;

	for (; n < EV_RPP; n++) {
		if (read_rec(p, n, &r) || rec_erased(&r)) {
			break;
		}
	}
	return n;
}

static void insert_sorted(const struct ev_page *pg)
{
	uint16_t i = m_nlive;

	while (i > 0 && m_pages[i - 1].seq > pg->seq) {
		m_pages[i] = m_pages[i - 1];
		i--;
	}
	m_pages[i] = *pg;
	m_nlive++;
}

static bool rec_is_event(const struct ev_page *p, uint8_t k, struct ev_rec *r)
{
	return read_rec(p, k, r) == 0 && r->mark == EV_REC_MARK && r->rule != EV_RULE_SENT;
}

/* Events at or after the upload position. */
static uint16_t count_unsent(void)
{
	uint16_t n = 0;

	for (uint16_t i = 0; i < m_nlive; i++) {
		for (uint8_t k = 0; k < m_pages[i].nrec; k++) {
			struct ev_rec r;

			if (page_abs(&m_pages[i], k) >= m_sent_abs &&
			    rec_is_event(&m_pages[i], k, &r)) {
				n++;
			}
		}
	}
	return n;
}

static void mount(void)
{
	m_nlive = 0;
	m_next_seq = 1;
	m_last_phys = m_npages - 1;

	for (uint16_t phys = 0; phys < m_npages; phys++) {
		struct ev_hdr h;
		struct ev_fixup f;

		if (flash_area_read(m_fa, page_off(phys), &h, sizeof(h)) || !hdr_valid(&h)) {
			continue;
		}
		struct ev_page pg = {.seq = h.seq, .phys = phys};

		if (h.synced) {
			pg.flags |= EVP_SYNCED;
		} else if (flash_area_read(m_fa, page_off(phys) + EV_FIXUP_OFF, &f, sizeof(f)) ==
				   0 &&
			   f.mark == EV_FIXUP_MARK &&
			   f.crc == crc16_ccitt(h.crc, (const uint8_t *)&f,
						offsetof(struct ev_fixup, crc))) {
			pg.offset = f.offset;
			pg.flags |= EVP_FIXED;
		}
		pg.nrec = scan_nrec(&pg);
		insert_sorted(&pg);
		if (h.seq >= m_next_seq) {
			m_next_seq = h.seq + 1;
			m_last_phys = phys;
		}
	}

	/* Upload position: the newest SENT record. */
	m_sent_abs = m_nlive ? page_abs(&m_pages[0], 0) : 0;
	for (uint16_t i = 0; i < m_nlive; i++) {
		for (uint8_t k = 0; k < m_pages[i].nrec; k++) {
			struct ev_rec r;

			if (read_rec(&m_pages[i], k, &r) == 0 && r.mark == EV_REC_MARK &&
			    r.rule == EV_RULE_SENT) {
				m_sent_abs = MAX(m_sent_abs, (uint32_t)r.value);
			}
		}
	}
	m_unsent = count_unsent();
	m_new = m_unsent;
	LOG_INF("Event log: %u page(s), %u unsent", m_nlive, m_unsent);
}

/* ---- append (writer work only) ------------------------------------------- */

/* Open a new head page (the oldest is erased when the ring is full). */
static int open_page(void)
{
	uint16_t phys = (m_last_phys + 1) % m_npages;

	if (m_nlive == m_npages) {
		/* Dropping the oldest page: its unsent events are lost. */
		struct ev_page *old = &m_pages[0];

		for (uint8_t k = 0; k < old->nrec; k++) {
			struct ev_rec r;

			if (page_abs(old, k) >= m_sent_abs && rec_is_event(old, k, &r)) {
				m_lost++;
				if (m_unsent) {
					m_unsent--;
				}
			}
		}
		memmove(&m_pages[0], &m_pages[1], (m_nlive - 1) * sizeof(m_pages[0]));
		m_nlive--;
		if (m_lost) {
			LOG_WRN("Event log full: %u unsent event(s) lost", m_lost);
		}
	}

	int ret = ev_erase(page_off(phys), EV_PAGE_SIZE);

	if (ret) {
		return ret;
	}

	uint32_t u;
	struct ev_hdr h = {
		.magic = EV_MAGIC,
		.seq = m_next_seq,
		.synced = now_unix(&u) ? 1 : 0,
		.split = split_pct(),
	};

	memset(h.rsv, 0xFF, sizeof(h.rsv));
	hdr_crc_set(&h);
	ret = ev_write(page_off(phys), &h, sizeof(h));
	if (ret) {
		return ret;
	}

	struct ev_page pg = {.seq = m_next_seq, .phys = phys, .flags = EVP_CUR_BOOT};

	if (h.synced) {
		pg.flags |= EVP_SYNCED;
	}
	m_pages[m_nlive++] = pg;
	m_next_seq++;
	m_last_phys = phys;
	m_head_open = true;
	if (m_sent_abs < page_abs(&m_pages[0], 0)) {
		m_sent_abs = page_abs(&m_pages[0], 0);
	}
	return 0;
}

static int append(const struct ev_rec *in)
{
	if (!m_head_open || m_nlive == 0 || m_pages[m_nlive - 1].nrec >= EV_RPP) {
		int ret = open_page();

		if (ret) {
			LOG_ERR_CALL_FAILED_INT("open_page", ret);
			return ret;
		}
	}

	struct ev_page *h = &m_pages[m_nlive - 1];
	struct ev_rec r = *in;

	r.mark = EV_REC_MARK;
	int ret = ev_write(page_off(h->phys) + EV_DATA_OFF + h->nrec * EV_REC_SIZE, &r, sizeof(r));

	h->nrec++; /* a failed write leaves a torn slot behind; it is skipped */
	return ret;
}

/* Program pending fix-ups of this boot's unsynced pages. */
static void flush_fixups(void)
{
	for (uint16_t i = 0; i < m_nlive; i++) {
		struct ev_page *p = &m_pages[i];
		struct ev_hdr h;

		if (!(p->flags & EVP_FIXUP_WR) ||
		    flash_area_read(m_fa, page_off(p->phys), &h, sizeof(h))) {
			continue;
		}
		struct ev_fixup f = {.offset = p->offset, .mark = EV_FIXUP_MARK, .rsv = 0};

		f.crc = crc16_ccitt(h.crc, (const uint8_t *)&f, offsetof(struct ev_fixup, crc));
		if (ev_write(page_off(p->phys) + EV_FIXUP_OFF, &f, sizeof(f)) == 0) {
			p->flags &= ~EVP_FIXUP_WR;
		}
	}
}

static void write_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&m_lock, K_FOREVER);
	flush_fixups();
	for (uint8_t i = 0; i < m_pend_n; i++) {
		const struct app_cmd_alarm_event *e = &m_pend[i].ev;
		uint32_t up = (uint32_t)(m_pend[i].at_ms / 1000);
		uint32_t u;
		bool synced_now = now_unix(&u);
		const struct ev_page *h = m_nlive ? &m_pages[m_nlive - 1] : NULL;
		/* The page the record lands in: the open head, or a new one stamped
		 * by the clock state now. */
		bool head_ok = h && m_head_open && h->nrec < EV_RPP;
		bool synced_page = head_ok ? (h->flags & EVP_SYNCED) != 0 : synced_now;
		uint32_t t = up;

		if (synced_page && synced_now) {
			t = u - (now_up_s() - up);
		}
		struct ev_rec r = {
			.time = t,
			.value = e->value,
			.rule = e->rule,
			.slot = e->slot,
			.channel = e->channel,
			.edge = e->edge,
			.type = e->type,
			.sensor_type = e->sensor_type,
			.has_value = e->has_value,
		};

		if (append(&r) == 0) {
			m_unsent++;
		}
	}
	m_pend_n = 0;
	k_mutex_unlock(&m_lock);
}

/* ---- upload --------------------------------------------------------------- */

/* Unix time of record `r` on page `p`, or its raw (uptime) time when unknown. */
static uint32_t rec_time(const struct ev_page *p, const struct ev_rec *r, bool *synced)
{
	*synced = true;
	if (p->flags & EVP_SYNCED) {
		return r->time;
	}
	if (p->flags & EVP_FIXED) {
		return r->time + p->offset;
	}
	*synced = false;
	return r->time;
}

/* Page holding ordinal `abs` (with its record index), NULL if none. */
static struct ev_page *page_of(uint32_t abs, uint8_t *idx)
{
	for (uint16_t i = 0; i < m_nlive; i++) {
		uint32_t first = page_abs(&m_pages[i], 0);

		if (abs >= first && abs < first + m_pages[i].nrec) {
			*idx = (uint8_t)(abs - first);
			return &m_pages[i];
		}
	}
	return NULL;
}

/* Next ordinal at or after `abs` that holds a record (skips page gaps). */
static bool next_abs(uint32_t abs, uint32_t *out)
{
	for (uint16_t i = 0; i < m_nlive; i++) {
		uint32_t first = page_abs(&m_pages[i], 0);

		if (abs < first + m_pages[i].nrec) {
			*out = MAX(abs, first);
			return true;
		}
	}
	return false;
}

/* Load the next chunk of the running upload into m_tx and lay out its pages.
 * A chunk never mixes synced and unsynced times, nor two unsynced pages (each
 * has its own boot's uptime epoch). Returns the event count. m_lock held. */
static uint16_t chunk_load(void)
{
	static uint32_t sel_t[EVENTLOG_CHUNK];
	uint16_t n = 0;
	bool have_dom = false, dom_synced = false;
	uint32_t dom_seq = 0;
	uint32_t abs;

	while (n < EVENTLOG_CHUNK && m_up_next_abs < m_up_end_abs &&
	       next_abs(m_up_next_abs, &abs) && abs < m_up_end_abs) {
		uint8_t idx;
		struct ev_page *p = page_of(abs, &idx);
		struct ev_rec r;
		bool synced;

		if (p == NULL || !rec_is_event(p, idx, &r)) {
			m_up_next_abs = abs + 1;
			continue;
		}
		uint32_t t = rec_time(p, &r, &synced);

		if (m_up_replay && synced && (t < m_up_from || t > m_up_to)) {
			m_up_next_abs = abs + 1;
			continue;
		}
		if (have_dom && (synced != dom_synced || (!synced && p->seq != dom_seq))) {
			break; /* next chunk */
		}
		have_dom = true;
		dom_synced = synced;
		dom_seq = p->seq;

		m_tx[n] = (struct app_cmd_alarm_event){
			.rule = r.rule,
			.slot = r.slot,
			.channel = r.channel,
			.edge = r.edge,
			.type = r.type,
			.sensor_type = r.sensor_type,
			.has_value = r.has_value,
			.value = r.value,
		};
		sel_t[n++] = t;
		m_up_next_abs = abs + 1;
	}

	m_tx_total = n;
	m_tx_pages = 0;
	m_tx_page = 0;
	m_tx_first = 0;
	if (n == 0) {
		return 0;
	}

	uint32_t base = UINT32_MAX;

	for (uint16_t i = 0; i < n; i++) {
		base = MIN(base, sel_t[i]);
	}
	for (uint16_t i = 0; i < n; i++) {
		m_tx[i].rel_s = sel_t[i] - base;
	}
	m_tx_base = base;
	m_tx_synced = dom_synced;

	size_t cap = EVENTLOG_FRAME_MAX;
	uint8_t dr = app_radio_get_max_payload();

	if (dr > 0 && dr < cap) {
		cap = dr;
	}

	uint8_t buf[EVENTLOG_FRAME_MAX];
	uint16_t laid = 0;

	while (laid < m_tx_total) {
		size_t len = 0;
		uint8_t k = MIN(m_tx_total - laid, EVENTLOG_PAGE_EVENTS);
		int ret = -EMSGSIZE;

		while (k > 0) {
			ret = app_cmd_build_alarm_report(m_tx_base, m_tx_total, m_tx_synced,
							 &m_tx[laid], k, EVENTLOG_PAGE_BOUND,
							 EVENTLOG_PAGE_BOUND, buf, cap, &len);
			if (ret == 0) {
				break;
			}
			k--;
		}
		if (ret != 0) {
			LOG_WRN("Event log: %u event(s) do not fit %u B; dropped",
				m_tx_total - laid, (unsigned)cap);
			break;
		}
		m_tx_page_n[m_tx_pages++] = k;
		laid += k;
	}

	LOG_INF("Event log %s: %u event(s) in %u page(s)", m_up_replay ? "replay" : "batch",
		m_tx_total, m_tx_pages);
	return n;
}

/* Queue the pages of the loaded chunk while answer slots are free. Returns true
 * when pages are left. m_lock held. */
static bool chunk_send(void)
{
	uint8_t buf[EVENTLOG_FRAME_MAX];

	while (m_tx_page < m_tx_pages) {
		if (app_radio_tx_answer_free() <= EVENTLOG_ANSWER_RESERVE) {
			return true;
		}

		uint8_t n = m_tx_page_n[m_tx_page];
		size_t len = 0;
		int ret = app_cmd_build_alarm_report(m_tx_base, m_tx_total, m_tx_synced,
						     &m_tx[m_tx_first], n, m_tx_page, m_tx_pages,
						     buf, sizeof(buf), &len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_cmd_build_alarm_report", ret);
			break;
		}

		ret = app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER,
					 APP_EVENTLOG_PORT, buf, len);
		if (ret == -ENOMEM) {
			return true;
		}
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_radio_tx_queue", ret);
		} else {
			LOG_INF("Event log page %u/%u: events %u..%u of %u (%u B)", m_tx_page + 1,
				m_tx_pages, m_tx_first + 1, m_tx_first + n, m_tx_total,
				(unsigned)len);
		}
		m_tx_first += n;
		m_tx_page++;
	}
	m_tx_pages = 0;
	m_tx_page = 0;
	return false;
}

/* Remember the upload position in flash (a SENT record). */
static void mark_sent(uint32_t abs)
{
	struct ev_rec r = {.time = now_up_s(), .value = (int32_t)abs, .rule = EV_RULE_SENT};

	m_sent_abs = abs;
	(void)append(&r);
	/* Edges stored while the batch streamed stay unsent. */
	m_unsent = count_unsent();
}

static void up_start(bool replay, uint32_t from, uint32_t to)
{
	m_up_active = true;
	m_up_replay = replay;
	m_up_from = from;
	m_up_to = to;
	m_up_next_abs = replay ? (m_nlive ? page_abs(&m_pages[0], 0) : 0) : m_sent_abs;
	m_up_end_abs = m_nlive ? page_abs(&m_pages[m_nlive - 1], 0) + m_pages[m_nlive - 1].nrec : 0;
	if (!replay) {
		m_new = 0;
	}
}

/* Advance the running upload. Returns true while it needs another run. */
static bool up_step(void)
{
	if (m_tx_page < m_tx_pages && chunk_send()) {
		return true;
	}
	while (m_up_active) {
		if (chunk_load() == 0) {
			if (!m_up_replay) {
				mark_sent(m_up_end_abs);
			}
			m_up_active = false;
			break;
		}
		if (chunk_send()) {
			return true;
		}
	}
	return false;
}

static void flush_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int64_t now = k_uptime_get();
	int interval = g_app_config.event_flush_interval;
	bool lorawan = g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_LORAWAN;
	k_timeout_t next = K_SECONDS(EVENTLOG_IDLE_POLL_S);

	k_mutex_lock(&m_lock, K_FOREVER);

	if (up_step()) {
		next = K_MSEC(EVENTLOG_RETRY_MS);
		goto out;
	}

	if (m_req && lorawan) {
		m_req = false;
		up_start(true, m_req_from, m_req_to);
		if (up_step()) {
			next = K_MSEC(EVENTLOG_RETRY_MS);
			goto out;
		}
	}

	int cnt = g_app_config.event_flush_count;
	bool count_due = cnt > 0 && m_new >= cnt;
	bool interval_due = interval > 0 && (now - m_last_flush_ms) >= (int64_t)interval * 1000;

	if ((m_force || count_due || interval_due) && m_unsent > 0 && lorawan) {
		up_start(false, 0, UINT32_MAX);
		m_last_flush_ms = now;
		m_force = false;
		if (up_step()) {
			next = K_MSEC(EVENTLOG_RETRY_MS);
			goto out;
		}
	} else if (interval_due) {
		m_last_flush_ms = now;
	}
	m_force = false;

	if (interval > 0) {
		int64_t left = m_last_flush_ms + (int64_t)interval * 1000 - now;

		next = K_MSEC(CLAMP(left, 1000, (int64_t)interval * 1000));
	}
out:
	k_mutex_unlock(&m_lock);
	k_work_reschedule_for_queue(m_q, &m_flush_work, next);
}

/* ---- API ------------------------------------------------------------------ */

void app_eventlog_add(const struct app_cmd_alarm_event *ev, int64_t at_ms)
{
	if (ev == NULL || !m_ready) {
		return;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	if (m_pend_n < EVENTLOG_PENDING) {
		m_pend[m_pend_n].ev = *ev;
		m_pend[m_pend_n].at_ms = at_ms > 0 ? at_ms : k_uptime_get();
		m_pend_n++;
	} else {
		m_lost++;
	}
	if (m_new < UINT16_MAX) {
		m_new++;
	}
	int cnt = g_app_config.event_flush_count;
	bool due = cnt > 0 && m_new >= cnt;

	k_mutex_unlock(&m_lock);

	LOG_INF("Event log: rule %u %s", ev->rule, ev->edge ? "deactivate" : "activate");

	k_work_submit_to_queue(m_q, &m_write_work);
	if (due) {
		/* After the writer work (same queue) has stored the edge. */
		k_work_reschedule_for_queue(m_q, &m_flush_work, K_MSEC(100));
	}
}

void app_eventlog_flush(void)
{
	if (!m_ready) {
		return;
	}
	k_mutex_lock(&m_lock, K_FOREVER);
	m_force = true;
	k_mutex_unlock(&m_lock);
	k_work_reschedule_for_queue(m_q, &m_flush_work, K_NO_WAIT);
}

int app_eventlog_request(uint32_t from_unix, uint32_t to_unix)
{
	if (g_app_config.radio_mode != APP_CONFIG_RADIO_MODE_LORAWAN) {
		return -ENOTSUP;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	if (!m_ready || m_nlive == 0) {
		k_mutex_unlock(&m_lock);
		return -ENODATA;
	}
	m_req = true;
	m_req_from = from_unix;
	m_req_to = to_unix;
	k_mutex_unlock(&m_lock);
	k_work_reschedule_for_queue(m_q, &m_flush_work, K_NO_WAIT);
	return 0;
}

void app_eventlog_on_clock_sync(uint32_t unix_now)
{
	if (!m_ready) {
		return;
	}

	uint32_t off = unix_now - now_up_s();

	k_mutex_lock(&m_lock, K_FOREVER);
	/* This boot's unsynced pages become unix time by the clock offset; the
	 * flash write is deferred to the writer work (no flash in the downlink
	 * callback, #96). Earlier boots' pages stay unsynced unless fixed. */
	for (uint16_t i = 0; i < m_nlive; i++) {
		struct ev_page *p = &m_pages[i];

		if ((p->flags & EVP_CUR_BOOT) && !(p->flags & (EVP_SYNCED | EVP_FIXED))) {
			p->offset = off;
			p->flags |= EVP_FIXED | EVP_FIXUP_WR;
		}
	}
	k_mutex_unlock(&m_lock);
	k_work_submit_to_queue(m_q, &m_write_work);
}

uint32_t app_eventlog_pending(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint32_t n = m_unsent + m_pend_n;
	k_mutex_unlock(&m_lock);
	return n;
}

uint32_t app_eventlog_lost(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint32_t n = m_lost;
	k_mutex_unlock(&m_lock);
	return n;
}

int app_eventlog_init(void)
{
	k_work_queue_init(&m_q_obj);
	k_work_queue_start(&m_q_obj, m_q_stack, K_THREAD_STACK_SIZEOF(m_q_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_work_init_delayable(&m_flush_work, flush_work_handler);
	k_work_init(&m_write_work, write_work_handler);

	if (!IS_ENABLED(CONFIG_APP_HISTORY_FLASH)) {
		return -ENOTSUP;
	}

	int ret = flash_area_open(FIXED_PARTITION_ID(history_partition), &m_fa);

	if (ret) {
		return ret;
	}

	/* A changed log_split re-lays the partition: erase it all (history too). */
	(void)settings_load_subtree("evlog");
	uint8_t split = split_pct();

	if (m_saved_split < 0) {
		m_saved_split = 0; /* firmware before the event log: all history */
	}
	if (m_saved_split != split) {
		LOG_WRN("log_split %d -> %u %%: erasing history + event log", m_saved_split, split);
		ret = ev_erase(0, FIXED_PARTITION_SIZE(history_partition));
		if (ret) {
			return ret;
		}
		ret = settings_save_one("evlog/split", &split, sizeof(split));
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("settings_save_one", ret);
		}
		m_saved_split = split;
	}

	m_npages = app_eventlog_flash_pages();
	if (m_npages < 2) {
		LOG_INF("Event log: no flash (log_split %u %%)", split);
		return -ENOSPC;
	}
	m_base = (off_t)(EV_TOTAL_PAGES - m_npages) * EV_PAGE_SIZE;

	k_mutex_lock(&m_lock, K_FOREVER);
	mount();
	m_ready = true;
	k_mutex_unlock(&m_lock);

	k_work_schedule_for_queue(m_q, &m_flush_work, K_SECONDS(EVENTLOG_IDLE_POLL_S));
	return 0;
}
