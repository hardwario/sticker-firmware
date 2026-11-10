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
#include <zephyr/sys/byteorder.h>
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
 * survive a reboot. Ordinal = page seq * EV_RPP + record index.
 *
 * Event seq: every event gets the next number of a global counter that never
 * restarts. A page header holds the seq of its first event (ev_first); the
 * following events of the page (every slot but SENT records, torn ones
 * included) count on by one, so no record grows. A jump in the numbering (an
 * edge dropped from the pending queue) opens a new page. After a reboot the
 * counter goes on from the head page's last seq + 1. */
#define EV_PAGE_SIZE   2048
#define EV_TOTAL_PAGES (FIXED_PARTITION_SIZE(history_partition) / EV_PAGE_SIZE)
#define EV_MAGIC       0x45564c32 /* "EVL2": header carries ev_first */
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
/* Edges waiting for the flash writer (alarm context never writes flash). The
 * writer runs at the lowest priority, so a burst from a higher-priority thread
 * (all 16 rules plus no-data watchdogs flipping in one poll) queues up here
 * before the first record is written: size for that, not for the average. */
#define EVENTLOG_PENDING        32

struct ev_hdr {
	uint32_t magic;
	uint32_t seq;
	uint8_t synced; /* 1: records carry unix time */
	uint8_t split;  /* log_split the ring was laid out for */
	uint32_t ev_first; /* seq of the page's first event */
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
	uint32_t ev_first; /* seq of the first event */
	uint16_t phys;
	uint8_t nrec; /* written records */
	uint8_t ev_n; /* event slots (records but SENT ones) */
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
static uint32_t m_ev_next;  /* seq of the next edge app_eventlog_add() takes */
static uint32_t m_ev_wr;    /* seq the next written event continues the head with */
static uint16_t m_unsent;   /* events at/after m_sent_abs */
static uint16_t m_new;      /* edges since the last flush (count trigger) */
static bool m_force;
static int64_t m_last_flush_ms;

/* Edges queued by app_eventlog_add() for the writer work. */
static struct {
	struct app_cmd_alarm_event ev;
	int64_t at_ms;
	uint32_t seq;
} m_pend[EVENTLOG_PENDING];
static uint8_t m_pend_n;

/* Upload in progress: a chunk of selected records, paged up front. */
static struct app_cmd_alarm_event m_tx[EVENTLOG_CHUNK];
static uint32_t m_tx_seq[EVENTLOG_CHUNK];
static uint8_t m_tx_page_n[EVENTLOG_CHUNK];
static uint8_t m_tx_fmt; /* enum app_config_event_format of the loaded chunk */
static uint16_t m_tx_total;
static uint16_t m_tx_first;
static uint8_t m_tx_pages;
static bool m_tx_nofit; /* not even one event fits the budget */

/* Pages handed to the radio, to re-send one the radio drops over budget. */
#define EVENTLOG_SENT_LOG 8
struct sent_page {
	uint32_t crc;
	uint32_t seq_first;
	uint32_t seq_last;
	bool auto_rp; /* a re-send of dropped frames: never re-sent again */
};
static struct sent_page m_sent_log[EVENTLOG_SENT_LOG];
static uint8_t m_sent_log_wr;
static uint8_t m_tx_page;
static uint32_t m_tx_base;
static bool m_tx_synced;

/* Scan cursor of the running upload. */
static bool m_up_active;
static bool m_up_replay;
static uint32_t m_up_next_abs; /* next ordinal to look at */
static uint32_t m_up_end_abs;  /* stop before this ordinal */
static bool m_up_any; /* the running replay found an event */

/* What a replay selects (req_events). */
enum sel_kind { SEL_TIME, SEL_RANGES, SEL_SEQS };
#define SEL_MAX_RANGES 8
#define SEL_MAX_SEQS   20
struct sel {
	uint8_t kind;
	uint8_t n;
	uint32_t from, to;              /* SEL_TIME */
	uint32_t rng[SEL_MAX_RANGES][2]; /* SEL_RANGES: seq_from, count */
	uint32_t seqs[SEL_MAX_SEQS];    /* SEL_SEQS */
};
static struct sel m_up_sel;
static bool m_req;
static struct sel m_req_sel;
/* Frames the radio dropped (over budget, abandoned): re-sent before m_req,
 * kept apart so a req_events downlink does not replace them. */
static bool m_drop;
static struct sel m_drop_sel;
static bool m_up_auto; /* the running replay re-sends dropped frames */

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
		struct ev_page pg = {.seq = h.seq, .ev_first = h.ev_first, .phys = phys};

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
		for (uint8_t k = 0; k < pg.nrec; k++) {
			struct ev_rec r;

			if (!(read_rec(&pg, k, &r) == 0 && r.mark == EV_REC_MARK &&
			      r.rule == EV_RULE_SENT)) {
				pg.ev_n++;
			}
		}
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
	/* Seq goes on after the newest event (1 on an empty ring). */
	m_ev_next = 1;
	if (m_nlive) {
		const struct ev_page *hd = &m_pages[m_nlive - 1];

		m_ev_next = MAX(hd->ev_first + hd->ev_n, 1);
	}
	m_ev_wr = m_ev_next;
	LOG_INF("Event log: %u page(s), %u unsent, next seq %u", m_nlive, m_unsent, m_ev_next);
}

/* ---- append (writer work only) ------------------------------------------- */

/* Open a new head page (the oldest is erased when the ring is full). */
static int open_page(uint32_t ev_first)
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
		.ev_first = ev_first,
	};

	hdr_crc_set(&h);
	ret = ev_write(page_off(phys), &h, sizeof(h));
	if (ret) {
		return ret;
	}

	struct ev_page pg = {
		.seq = m_next_seq, .ev_first = ev_first, .phys = phys, .flags = EVP_CUR_BOOT};

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

/* Append a record; an event takes `seq` (ignored for SENT records). */
static int append(const struct ev_rec *in, uint32_t seq)
{
	bool event = in->rule != EV_RULE_SENT;

	if (!m_head_open || m_nlive == 0 || m_pages[m_nlive - 1].nrec >= EV_RPP ||
	    (event && seq != m_ev_wr)) {
		if (event && seq != m_ev_wr) {
			LOG_WRN("Event log: seq %u..%u missing", m_ev_wr, seq - 1);
		}
		int ret = open_page(event ? seq : m_ev_wr);

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
	if (event) {
		h->ev_n++; /* a torn event slot still holds its seq */
		m_ev_wr = seq + 1;
	}
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
		/* The page the record lands in: the open head, or a new one (head
		 * full, closed or a seq jump, see append()) stamped by the clock now. */
		bool head_ok = h && m_head_open && h->nrec < EV_RPP && m_pend[i].seq == m_ev_wr;
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

		if (append(&r, m_pend[i].seq) == 0) {
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

/* Seq of record `idx` on page `p`: ev_first + the event slots before it. The
 * upload walks records in order, so the last answer is cached. m_lock held. */
static uint32_t rec_seq(const struct ev_page *p, uint8_t idx)
{
	static uint32_t c_page = UINT32_MAX;
	static uint8_t c_idx;
	static uint32_t c_val;
	uint8_t k = 0;
	uint32_t v = p->ev_first;

	if (c_page == p->seq && c_idx <= idx) {
		k = c_idx;
		v = c_val;
	}
	for (; k < idx; k++) {
		struct ev_rec r;

		if (!(read_rec(p, k, &r) == 0 && r.mark == EV_REC_MARK &&
		      r.rule == EV_RULE_SENT)) {
			v++;
		}
	}
	c_page = p->seq;
	c_idx = idx;
	c_val = v;
	return v;
}

/* Oldest retained seq (the next one when the ring holds no event). */
static uint32_t oldest_seq(void)
{
	return m_nlive ? m_pages[0].ev_first : m_ev_next;
}

/* Does the running replay want this event? */
static bool sel_match(const struct sel *q, uint32_t seq, uint32_t t, bool synced)
{
	switch (q->kind) {
	case SEL_RANGES:
		for (uint8_t i = 0; i < q->n; i++) {
			if (seq >= q->rng[i][0] && seq - q->rng[i][0] < q->rng[i][1]) {
				return true;
			}
		}
		return false;
	case SEL_SEQS:
		for (uint8_t i = 0; i < q->n; i++) {
			if (q->seqs[i] == seq) {
				return true;
			}
		}
		return false;
	default:
		/* Unsynced times cannot be compared: always sent */
		return !synced || (t >= q->from && t <= q->to);
	}
}

/* Raw format (fPort 4, version 0x03): u8 version, u32 LE base_time, u8 flags,
 * u32 LE seq_first, [u32 LE oldest_seq when replay], then per event: u16 LE
 * rel_s, u8 edge << 7 | rule (0..15, 126 = low battery, 127 = watchdog),
 * [u8 seq delta from the previous event when flag bit 2]. Flags: bit 0 = time
 * synced, bit 1 = replay, bit 2 = per-event seq delta. */
#define EVENTLOG_RAW_VERSION 0x03
#define EVENTLOG_RAW_HDR     10
#define EVENTLOG_RAW_HDR_RP  14
#define EVENTLOG_RAW_REC     3
#define EVENTLOG_RAW_F_SYNC  BIT(0)
#define EVENTLOG_RAW_F_RP    BIT(1)
#define EVENTLOG_RAW_F_DELTA BIT(2)
/* Compact format (version 0x02): AlarmReport with only rel_s / edge / rule. */
#define EVENTLOG_COMPACT_VERSION 0x02

static uint8_t raw_rule(uint8_t rule)
{
	return rule == 0xFF ? 127 : rule == 0xFE ? 126 : (rule & 0x7F);
}

/* Build one fPort-4 page of the loaded chunk's events [first, first + n) in
 * its format (n = 0: an empty replay answer). */
static int page_build(uint16_t first, uint8_t n, uint32_t page_index, uint32_t page_count,
		      uint8_t *buf, size_t cap, size_t *len)
{
	const struct app_cmd_alarm_event *ev = &m_tx[first];
	const uint32_t *sq = &m_tx_seq[first];
	uint32_t seq_first = n ? sq[0] : m_ev_next;
	bool gaps = false;

	for (uint8_t i = 1; i < n; i++) {
		gaps |= sq[i] != sq[i - 1] + 1;
	}

	if (m_tx_fmt == APP_CONFIG_EVENT_FORMAT_RAW) {
		size_t hdr = m_up_replay ? EVENTLOG_RAW_HDR_RP : EVENTLOG_RAW_HDR;
		size_t rec = EVENTLOG_RAW_REC + (gaps ? 1 : 0);
		size_t need = hdr + (size_t)n * rec;

		if (need > cap) {
			return -EMSGSIZE;
		}
		buf[0] = EVENTLOG_RAW_VERSION;
		sys_put_le32(m_tx_base, &buf[1]);
		buf[5] = (m_tx_synced ? EVENTLOG_RAW_F_SYNC : 0) |
			 (m_up_replay ? EVENTLOG_RAW_F_RP : 0) | (gaps ? EVENTLOG_RAW_F_DELTA : 0);
		sys_put_le32(seq_first, &buf[6]);
		if (m_up_replay) {
			sys_put_le32(oldest_seq(), &buf[10]);
		}
		for (uint8_t i = 0; i < n; i++) {
			uint8_t *r = &buf[hdr + i * rec];

			sys_put_le16((uint16_t)ev[i].rel_s, r);
			r[2] = (uint8_t)((ev[i].edge ? 0x80 : 0x00) | raw_rule(ev[i].rule));
			if (gaps) {
				uint32_t d = i ? sq[i] - sq[i - 1] : 0;

				if (d > UINT8_MAX) {
					return -EMSGSIZE; /* split the page here */
				}
				r[3] = (uint8_t)d;
			}
		}
		*len = need;
		return 0;
	}

	/* Own seq only where the +1 run breaks */
	for (uint8_t i = 0; i < n; i++) {
		m_tx[first + i].seq = (i && sq[i] != sq[i - 1] + 1) ? sq[i] : 0;
	}
	struct app_cmd_alarm_seq aseq = {
		.seq_first = seq_first, .oldest = oldest_seq(), .replay = m_up_replay};
	int ret = app_cmd_build_alarm_report_seq(m_tx_base, m_tx_total, m_tx_synced, ev, n,
						 page_index, page_count, &aseq, buf, cap, len);

	if (ret == 0 && m_tx_fmt == APP_CONFIG_EVENT_FORMAT_COMPACT) {
		buf[0] = EVENTLOG_COMPACT_VERSION;
	}
	return ret;
}

/* Events one page may hold in the loaded chunk's format. */
static uint8_t page_max_events(size_t cap)
{
	if (m_tx_fmt == APP_CONFIG_EVENT_FORMAT_RAW) {
		size_t hdr = m_up_replay ? EVENTLOG_RAW_HDR_RP : EVENTLOG_RAW_HDR;

		return cap <= hdr ? 0 : (uint8_t)MIN((cap - hdr) / EVENTLOG_RAW_REC, UINT8_MAX);
	}
	return EVENTLOG_PAGE_EVENTS;
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
	uint32_t t_min = UINT32_MAX, t_max = 0;

	m_tx_fmt = (uint8_t)g_app_config.event_format;

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
		uint32_t seq = rec_seq(p, idx);

		if (m_up_replay && !sel_match(&m_up_sel, seq, t, synced)) {
			m_up_next_abs = abs + 1;
			continue;
		}
		if (have_dom && (synced != dom_synced || (!synced && p->seq != dom_seq))) {
			break; /* next chunk */
		}
		/* Raw stamps rel_s on 16 bits: keep a chunk within ~18 h */
		if (m_tx_fmt == APP_CONFIG_EVENT_FORMAT_RAW && n > 0 &&
		    MAX(t_max, t) - MIN(t_min, t) > UINT16_MAX) {
			break;
		}
		t_min = MIN(t_min, t);
		t_max = MAX(t_max, t);
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
		if (m_tx_fmt != APP_CONFIG_EVENT_FORMAT_FULL) {
			/* time / edge / rule only: the rule index identifies the rest */
			m_tx[n] = (struct app_cmd_alarm_event){.rule = r.rule, .edge = r.edge};
		}
		m_tx_seq[n] = seq;
		sel_t[n++] = t;
		m_up_next_abs = abs + 1;
	}

	m_tx_total = n;
	m_tx_nofit = false;
	m_tx_pages = 0;
	m_tx_page = 0;
	m_tx_first = 0;
	if (n == 0) {
		return 0;
	}
	m_up_any = true;

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
		uint8_t k = MIN(m_tx_total - laid, page_max_events(cap));
		int ret = -EMSGSIZE;

		while (k > 0) {
			ret = page_build(laid, k, EVENTLOG_PAGE_BOUND, EVENTLOG_PAGE_BOUND, buf,
					 cap, &len);
			if (ret == 0) {
				break;
			}
			k--;
		}
		if (ret != 0) {
			LOG_WRN("Event log: no event fits %u B; upload deferred", (unsigned)cap);
			m_tx_nofit = true;
			m_tx_pages = 0;
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
		int ret = page_build(m_tx_first, n, m_tx_page, m_tx_pages, buf, sizeof(buf),
				     &len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("page_build", ret);
			break;
		}

		ret = app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_EVENTLOG,
					 APP_EVENTLOG_PORT, buf, len);
		if (ret == -ENOMEM) {
			return true;
		}
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_radio_tx_queue", ret);
		} else {
			m_sent_log[m_sent_log_wr] = (struct sent_page){
				.crc = crc32_ieee(buf, len),
				.seq_first = m_tx_seq[m_tx_first],
				.seq_last = m_tx_seq[m_tx_first + n - 1],
				.auto_rp = m_up_auto,
			};
			m_sent_log_wr = (m_sent_log_wr + 1) % EVENTLOG_SENT_LOG;
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
	(void)append(&r, 0);
	/* Edges stored while the batch streamed stay unsent. */
	m_unsent = count_unsent();
}

/* A replay that found nothing still answers: an empty page with oldest_seq. */
static void send_empty_replay(void)
{
	uint8_t buf[EVENTLOG_FRAME_MAX];
	size_t len = 0;

	m_tx_fmt = (uint8_t)g_app_config.event_format;
	m_tx_total = 0;
	m_tx_base = 0;
	m_tx_synced = false;
	if (page_build(0, 0, 0, 1, buf, sizeof(buf), &len) == 0 &&
	    app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, APP_EVENTLOG_PORT, buf,
			       len) == 0) {
		LOG_INF("Event log replay: nothing selected (oldest seq %u)", oldest_seq());
	}
}

static void up_start(bool replay, const struct sel *q)
{
	m_up_active = true;
	m_up_replay = replay;
	m_up_any = false;
	if (q) {
		m_up_sel = *q;
	}
	m_up_next_abs = replay ? (m_nlive ? page_abs(&m_pages[0], 0) : 0) : m_sent_abs;
	m_up_end_abs = m_nlive ? page_abs(&m_pages[m_nlive - 1], 0) + m_pages[m_nlive - 1].nrec : 0;
	if (!replay) {
		m_new = 0;
		m_up_auto = false;
	}
}

/* Advance the running upload. Returns true while it needs another run. */
static bool up_step(void)
{
	if (m_tx_page < m_tx_pages && chunk_send()) {
		return true;
	}
	while (m_up_active) {
		uint16_t loaded = chunk_load();

		if (loaded > 0 && m_tx_nofit) {
			/* Budget too small for one event (e.g. US915 DR0): keep the
			 * events unsent, the next flush tries again. */
			m_up_active = false;
			if (!m_up_replay) {
				m_new = MAX(m_new, 1);
			}
			break;
		}
		if (loaded == 0) {
			if (!m_up_replay) {
				mark_sent(m_up_end_abs);
			} else if (!m_up_any) {
				send_empty_replay();
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

	if ((m_drop || m_req) && lorawan) {
		struct sel q;

		m_up_auto = m_drop;
		if (m_drop) {
			q = m_drop_sel;
			m_drop = false;
		} else {
			q = m_req_sel;
			m_req = false;
		}
		up_start(true, &q);
		if (up_step() || m_drop || m_req) {
			next = K_MSEC(EVENTLOG_RETRY_MS);
			goto out;
		}
	}

	int cnt = g_app_config.event_flush_count;
	bool count_due = cnt > 0 && m_new >= cnt;
	bool interval_due = interval > 0 && (now - m_last_flush_ms) >= (int64_t)interval * 1000;

	if ((m_force || count_due || interval_due) && m_unsent > 0 && lorawan) {
		up_start(false, NULL);
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
		m_pend[m_pend_n].seq = m_ev_next;
		m_pend_n++;
	} else {
		m_lost++;
	}
	m_ev_next++; /* a dropped edge leaves its seq out: a visible gap */
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

static int request(const struct sel *q)
{
	if (g_app_config.radio_mode != APP_CONFIG_RADIO_MODE_LORAWAN) {
		return -ENOTSUP;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	if (!m_ready) {
		k_mutex_unlock(&m_lock);
		return -ENODATA;
	}
	m_req = true;
	m_req_sel = *q;
	k_mutex_unlock(&m_lock);
	k_work_reschedule_for_queue(m_q, &m_flush_work, K_NO_WAIT);
	return 0;
}

int app_eventlog_request(uint32_t from_unix, uint32_t to_unix)
{
	struct sel q = {.kind = SEL_TIME, .from = from_unix, .to = to_unix};

	return request(&q);
}

int app_eventlog_request_ranges(const uint32_t *pairs, size_t n_pairs)
{
	struct sel q = {.kind = SEL_RANGES};

	if (n_pairs == 0 || n_pairs > SEL_MAX_RANGES) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n_pairs; i++) {
		q.rng[i][0] = pairs[2 * i];
		q.rng[i][1] = pairs[2 * i + 1];
	}
	q.n = (uint8_t)n_pairs;
	return request(&q);
}

int app_eventlog_request_seqs(const uint32_t *seqs, size_t n)
{
	struct sel q = {.kind = SEL_SEQS};

	if (n == 0 || n > SEL_MAX_SEQS) {
		return -EINVAL;
	}
	memcpy(q.seqs, seqs, n * sizeof(seqs[0]));
	q.n = (uint8_t)n;
	return request(&q);
}

void app_eventlog_frame_dropped(const uint8_t *buf, size_t len)
{
	uint32_t crc = crc32_ieee(buf, len);
	bool queued = false;
	int i;

	k_mutex_lock(&m_lock, K_FOREVER);
	for (i = 0; i < EVENTLOG_SENT_LOG; i++) {
		if (m_sent_log[i].seq_last != 0 && m_sent_log[i].crc == crc) {
			break;
		}
	}
	if (i < EVENTLOG_SENT_LOG) {
		struct sent_page *sp = &m_sent_log[i];
		uint32_t first = sp->seq_first, cnt = sp->seq_last - sp->seq_first + 1;

		sp->seq_last = 0;
		/* Several pages dropped in a row join one ranges request. A
		 * re-send that is dropped again is left to the receiver (seq gap),
		 * so a dead link does not loop. */
		if (!m_drop) {
			m_drop_sel = (struct sel){.kind = SEL_RANGES};
		}
		if (!sp->auto_rp && m_drop_sel.n < SEL_MAX_RANGES) {
			m_drop_sel.rng[m_drop_sel.n][0] = first;
			m_drop_sel.rng[m_drop_sel.n][1] = cnt;
			m_drop_sel.n++;
			m_drop = true;
			queued = true;
		}
		LOG_WRN("Event log page seq %u..%u dropped by the radio: %s", first,
			first + cnt - 1, queued ? "replay" : "gap left");
	} else {
		LOG_WRN("Event log page dropped by the radio: unknown, gap left");
	}
	k_mutex_unlock(&m_lock);
	if (queued) {
		k_work_reschedule_for_queue(m_q, &m_flush_work, K_NO_WAIT);
	}
}

int app_eventlog_export(uint32_t start_seq, uint32_t from_unix, uint32_t to_unix,
			struct app_cmd_alarm_event *ev, size_t max, struct app_eventlog_page *page)
{
	struct sel q = {.kind = SEL_TIME, .from = from_unix, .to = to_unix};
	uint32_t times[EVENTLOG_PAGE_EVENTS];
	uint32_t seqs[EVENTLOG_PAGE_EVENTS];
	bool have_dom = false, dom_synced = false;
	uint32_t dom_page = 0;
	size_t n = 0;

	max = MIN(max, ARRAY_SIZE(times));
	memset(page, 0, sizeof(*page));

	k_mutex_lock(&m_lock, K_FOREVER);
	if (!m_ready) {
		k_mutex_unlock(&m_lock);
		return -ENODATA;
	}
	page->oldest = oldest_seq();
	page->next_seq = m_ev_next;

	for (uint16_t i = 0; i < m_nlive; i++) {
		const struct ev_page *p = &m_pages[i];

		if (p->ev_n == 0 || p->ev_first + p->ev_n <= start_seq) {
			continue; /* whole page before the cursor */
		}
		for (uint8_t k = 0; k < p->nrec; k++) {
			struct ev_rec r;
			bool synced;

			if (!rec_is_event(p, k, &r)) {
				continue;
			}
			uint32_t seq = rec_seq(p, k);
			uint32_t t = rec_time(p, &r, &synced);

			if (seq < start_seq || !sel_match(&q, seq, t, synced)) {
				continue;
			}
			if (n == max ||
			    (have_dom && (synced != dom_synced || (!synced && p->seq != dom_page)))) {
				/* Page full: the next one starts here */
				page->next_seq = seq;
				page->more = true;
				goto done;
			}
			have_dom = true;
			dom_synced = synced;
			dom_page = p->seq;
			ev[n] = (struct app_cmd_alarm_event){
				.rule = r.rule,
				.slot = r.slot,
				.channel = r.channel,
				.edge = r.edge,
				.type = r.type,
				.sensor_type = r.sensor_type,
				.has_value = r.has_value,
				.value = r.value,
			};
			times[n] = t;
			seqs[n++] = seq;
		}
	}
done:
	k_mutex_unlock(&m_lock);

	uint32_t base = UINT32_MAX;

	for (size_t i = 0; i < n; i++) {
		base = MIN(base, times[i]);
	}
	for (size_t i = 0; i < n; i++) {
		ev[i].rel_s = times[i] - base;
		ev[i].seq = (i && seqs[i] != seqs[i - 1] + 1) ? seqs[i] : 0;
	}
	page->base_time = n ? base : 0;
	page->synced = dom_synced;
	page->seq_first = n ? seqs[0] : page->next_seq;
	return (int)n;
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
	static bool q_started;

	if (!q_started) {
		k_work_queue_init(&m_q_obj);
		k_work_queue_start(&m_q_obj, m_q_stack, K_THREAD_STACK_SIZEOF(m_q_stack),
				   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
		k_work_init_delayable(&m_flush_work, flush_work_handler);
		k_work_init(&m_write_work, write_work_handler);
		q_started = true;
	}

	/* A second init (tests: a simulated reboot) starts from a clean RAM state. */
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&m_flush_work, &sync);
	(void)k_work_flush(&m_write_work, &sync);
	k_mutex_lock(&m_lock, K_FOREVER);
	m_ready = false;
	m_head_open = false;
	m_pend_n = 0;
	m_lost = 0;
	m_force = false;
	m_req = false;
	m_drop = false;
	m_up_auto = false;
	m_up_active = false;
	memset(m_sent_log, 0, sizeof(m_sent_log));
	m_tx_pages = 0;
	m_tx_page = 0;
	m_last_flush_ms = k_uptime_get();
	k_mutex_unlock(&m_lock);

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

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <stdlib.h>

static int cmd_eventlog_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&m_lock, K_FOREVER);
	shell_print(sh, "ready %d pages %u live %u unsent %u queued %u lost %u sent_abs %u "
		    "upload %d replay %d format %d seq next %u oldest %u",
		    m_ready, m_npages, m_nlive, m_unsent, m_pend_n, m_lost, m_sent_abs,
		    m_up_active, m_up_replay, g_app_config.event_format, m_ev_next,
		    oldest_seq());
	for (uint16_t i = 0; i < m_nlive; i++) {
		shell_print(sh, "  page %u phys %u nrec %u ev %u..%u", m_pages[i].seq,
			    m_pages[i].phys, m_pages[i].nrec, m_pages[i].ev_first,
			    m_pages[i].ev_first + m_pages[i].ev_n - 1);
	}
	k_mutex_unlock(&m_lock);
	return 0;
}

static int cmd_eventlog_flush(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_eventlog_flush();
	shell_print(sh, "flush requested");
	return 0;
}

/* eventlog page [start_seq]: what one NFC read returns */
static int cmd_eventlog_page(const struct shell *sh, size_t argc, char **argv)
{
	struct app_cmd_alarm_event ev[6];
	struct app_eventlog_page pg;
	uint32_t start = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
	int n = app_eventlog_export(start, 0, UINT32_MAX, ev, ARRAY_SIZE(ev), &pg);

	shell_print(sh, "n %d first %u oldest %u next %u more %d synced %d base %u", n,
		    pg.seq_first, pg.oldest, pg.next_seq, pg.more, pg.synced, pg.base_time);
	for (int i = 0; i < n; i++) {
		shell_print(sh, "  rule %u edge %u rel %u own_seq %u", ev[i].rule, ev[i].edge,
			    ev[i].rel_s, ev[i].seq);
	}
	return 0;
}

/* eventlog req time <from> <to> | rng <seq> <count> [...] | seq <seq> [...] */
static int cmd_eventlog_req(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t v[MAX(2 * SEL_MAX_RANGES, SEL_MAX_SEQS)];
	size_t n = argc - 2;
	int ret;

	if (argc < 3 || n > ARRAY_SIZE(v)) {
		shell_error(sh, "usage: req time <from> <to> | rng <seq> <count>... | seq <seq>...");
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		v[i] = (uint32_t)strtoul(argv[i + 2], NULL, 0);
	}
	if (strcmp(argv[1], "time") == 0 && n == 2) {
		ret = app_eventlog_request(v[0], v[1]);
	} else if (strcmp(argv[1], "rng") == 0 && n % 2 == 0) {
		ret = app_eventlog_request_ranges(v, n / 2);
	} else if (strcmp(argv[1], "seq") == 0) {
		ret = app_eventlog_request_seqs(v, n);
	} else {
		ret = -EINVAL;
	}
	shell_print(sh, "req: %d", ret);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_eventlog,
			       SHELL_CMD_ARG(req, NULL, "Replay: time|rng|seq ...",
					     cmd_eventlog_req, 3, SEL_MAX_SEQS - 1),
			       SHELL_CMD(info, NULL, "Event log state.", cmd_eventlog_info),
			       SHELL_CMD_ARG(page, NULL, "NFC page from [seq].", cmd_eventlog_page,
					     1, 1),
			       SHELL_CMD(flush, NULL, "Upload pending events now.",
					 cmd_eventlog_flush),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(eventlog, &sub_eventlog, "Event log (fPort 4).", NULL);
#endif /* defined(CONFIG_SHELL) */
