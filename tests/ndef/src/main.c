/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_nfc_parser, covering the NFC Forum Type-5 Capability
 * Container (CC) skip (#218) and the #199 unknown-TLV value-skip. Vectors use
 * the device's own canonical record: NDEF Message TLV (0x03) carrying a short
 * external-type record `hio.stck:cfg` with a 4-byte payload, then a terminator.
 */

#include "app_nfc_parser.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* "hio.stck:cfg" (12 B external type) + payload 00 01 02 03 (4 B). */
#define TYPE_HEX    "68696f2e7374636b3a636667"
#define TYPE_STR    "hio.stck:cfg"
#define PAYLOAD_HEX "00010203"

/* TLV area, no CC: NDEF Message TLV (03) len 0x13, record d4/0c/04, terminator. */
#define TLV_AREA "0313d40c0468696f2e7374636b3a63666700010203fe"

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};

		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}

	return n;
}

static int m_cb_count;
static uint8_t m_type[32];
static uint32_t m_type_len;
static uint8_t m_payload[32];
static uint32_t m_payload_len;

static int capture_cb(const struct app_nfc_parser_record_info *info, void *user_data)
{
	ARG_UNUSED(user_data);
	m_cb_count++;
	m_type_len = info->type_len;
	m_payload_len = info->payload_len;
	if (info->type_len <= sizeof(m_type)) {
		memcpy(m_type, info->type, info->type_len);
	}
	if (info->payload_len <= sizeof(m_payload)) {
		memcpy(m_payload, info->payload, info->payload_len);
	}
	return 0;
}

static void reset_capture(void *fixture)
{
	ARG_UNUSED(fixture);
	m_cb_count = 0;
	m_type_len = 0;
	m_payload_len = 0;
	memset(m_type, 0, sizeof(m_type));
	memset(m_payload, 0, sizeof(m_payload));
}

/* Assert the canonical hio.stck:cfg record was delivered exactly once. */
static void expect_canonical_record(void)
{
	uint8_t exp_type[16], exp_payload[8];
	size_t exp_type_len = unhex(TYPE_HEX, exp_type, sizeof(exp_type));
	size_t exp_payload_len = unhex(PAYLOAD_HEX, exp_payload, sizeof(exp_payload));

	zassert_equal(m_cb_count, 1, "expected exactly one record, got %d", m_cb_count);
	zassert_equal(m_type_len, exp_type_len, "type len");
	zassert_mem_equal(m_type, exp_type, exp_type_len, "type bytes (%s)", TYPE_STR);
	zassert_equal(m_payload_len, exp_payload_len, "payload len");
	zassert_mem_equal(m_payload, exp_payload, exp_payload_len, "payload bytes");
}

static int run_hex(const char *hex)
{
	uint8_t buf[128];
	size_t len = unhex(hex, buf, sizeof(buf));

	return app_nfc_parser_run(buf, len, capture_cb, NULL);
}

ZTEST_SUITE(ndef, NULL, NULL, reset_capture, NULL, NULL);

/* A raw TLV area written from offset 0 (no CC magic) parses unchanged. */
ZTEST(ndef, test_no_cc_offset0)
{
	zassert_ok(run_hex(TLV_AREA));
	expect_canonical_record();
}

/* #218: behind the standard 4-byte CC (E1 40 40 01) the record must still be
 * found — previously the CC was eaten as a 0x40-long unknown TLV, skipping it. */
ZTEST(ndef, test_cc_4byte_skipped)
{
	zassert_ok(run_hex("e1404001" TLV_AREA));
	expect_canonical_record();
}

/* 8-byte (extended) CC form: MLEN byte is 0, real length lives in bytes 6-7. */
ZTEST(ndef, test_cc_8byte_skipped)
{
	zassert_ok(run_hex("e1400001000000ff" TLV_AREA));
	expect_canonical_record();
}

/* #199 must still hold: an unknown TLV before the NDEF TLV is skipped by its
 * declared length, not byte-by-byte. Here a CC + unknown TLV (c1 len 2) precede. */
ZTEST(ndef, test_cc_then_unknown_tlv_then_ndef)
{
	zassert_ok(run_hex("e1404001"
			   "c102aabb" TLV_AREA));
	expect_canonical_record();
}

/* CC followed immediately by a terminator: no NDEF message, no callback. */
ZTEST(ndef, test_cc_then_terminator)
{
	zassert_ok(run_hex("e1404001fe"));
	zassert_equal(m_cb_count, 0, "no record expected");
}

/* CC present but no TLV area at all (truncated): graceful, no record. */
ZTEST(ndef, test_cc_only_no_tlv)
{
	int ret = run_hex("e1404001");

	zassert_true(ret == 0 || ret == -ENOMSG, "got %d", ret);
	zassert_equal(m_cb_count, 0, "no record expected");
}

/* ---- #247 multi-record (inf + clm) ------------------------------------- */

/* Append one NFC Forum external-type short record, mirroring the byte layout of
 * app_nfc.c build_ndef_message(): flags (MB/ME/SR|TNF=0x04), type_len, payload_len,
 * type, payload. */
static void emit_ext_record(uint8_t *buf, size_t *off, const char *type, const uint8_t *payload,
			    size_t plen, bool mb, bool me)
{
	uint8_t flags = 0x04; /* TNF external */
	if (mb) {
		flags |= 0x80;
	}
	if (me) {
		flags |= 0x40;
	}
	flags |= 0x10; /* short record (payloads here are < 256 B) */
	buf[(*off)++] = flags;
	buf[(*off)++] = (uint8_t)strlen(type);
	buf[(*off)++] = (uint8_t)plen;
	memcpy(&buf[*off], type, strlen(type));
	*off += strlen(type);
	memcpy(&buf[*off], payload, plen);
	*off += plen;
}

#define MAX_REC 4
static int m_multi_n;
static char m_multi_type[MAX_REC][16];
static uint32_t m_multi_plen[MAX_REC];

static int multi_cb(const struct app_nfc_parser_record_info *info, void *user_data)
{
	ARG_UNUSED(user_data);
	if (m_multi_n < MAX_REC && info->type_len < sizeof(m_multi_type[0])) {
		memcpy(m_multi_type[m_multi_n], info->type, info->type_len);
		m_multi_type[m_multi_n][info->type_len] = '\0';
		m_multi_plen[m_multi_n] = info->payload_len;
	}
	m_multi_n++;
	return 0;
}

/* A two-record message [inf, clm] (MB on inf, ME on clm) parses back into both
 * records in order with the right types and payload lengths — the wire shape the
 * firmware lays down during the provisioning window (#247). */
ZTEST(ndef, test_multi_record_inf_clm)
{
	uint8_t inf[15] = {0x02, 0, 0, 0, 1};       /* format ver + serial=1 (rest zero) */
	uint8_t clm[20] = {0x08, 0x2a, 0x12, 0x10}; /* ClaimInfo-ish: serial + 16 B token */

	uint8_t msg[128];
	size_t moff = 0;
	emit_ext_record(msg, &moff, "hio.stck:inf", inf, sizeof(inf), true, false);
	emit_ext_record(msg, &moff, "hio.stck:clm", clm, sizeof(clm), false, true);

	uint8_t tag[160];
	size_t off = 0;
	tag[off++] = 0xE1; /* CC */
	tag[off++] = 0x40;
	tag[off++] = 0x40;
	tag[off++] = 0x01;
	tag[off++] = 0x03;          /* NDEF Message TLV */
	tag[off++] = (uint8_t)moff; /* msg_len < 0xFF */
	memcpy(&tag[off], msg, moff);
	off += moff;
	tag[off++] = 0xFE; /* terminator */

	m_multi_n = 0;
	zassert_ok(app_nfc_parser_run(tag, off, multi_cb, NULL));
	zassert_equal(m_multi_n, 2, "expected 2 records, got %d", m_multi_n);
	zassert_str_equal(m_multi_type[0], "hio.stck:inf", "record 0 type");
	zassert_equal(m_multi_plen[0], 15, "inf payload len");
	zassert_str_equal(m_multi_type[1], "hio.stck:clm", "record 1 type");
	zassert_equal(m_multi_plen[1], 20, "clm payload len");
}
