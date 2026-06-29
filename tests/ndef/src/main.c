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
