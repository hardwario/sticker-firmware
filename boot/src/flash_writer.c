/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_writer.h"

#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#define SLOT0_ID FIXED_PARTITION_ID(slot0_partition)
#define SLOT0_OFFSET FIXED_PARTITION_OFFSET(slot0_partition)
#define SLOT0_SIZE FIXED_PARTITION_SIZE(slot0_partition)

#define FLASH_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_flash))

/*
 * sfu_meta lives in its own fixed partition. The DTS partition redesign
 * (doc §7, Phase 2 remaining work) must add `sfu_meta_partition`; until then
 * this references it so the dependency is explicit.
 */
#define META_ID FIXED_PARTITION_ID(sfu_meta_partition)
#define META_SIZE FIXED_PARTITION_SIZE(sfu_meta_partition)

#define CRC_CHUNK 256

/* STM32WLE5 main flash: uniform 2 KB pages (the erase granularity). */
#define FW_PAGE_SZ  2048
#define FW_MAX_PAGES ((SLOT0_SIZE + FW_PAGE_SZ - 1) / FW_PAGE_SZ)

/* Incremental-erase bookkeeping: one bit per slot page, set once the page has
 * been erased during this session. */
static uint8_t m_erased[(FW_MAX_PAGES + 7) / 8];
static bool m_incremental;

void fw_begin_incremental(void)
{
	memset(m_erased, 0, sizeof(m_erased));
	m_incremental = true;
}

/* Erase any not-yet-erased page(s) covering [off, off+len). */
static int ensure_erased(const struct flash_area *fa, uint32_t off, size_t len)
{
	uint32_t first = off / FW_PAGE_SZ;
	uint32_t last = (off + len - 1) / FW_PAGE_SZ;

	for (uint32_t p = first; p <= last && p < FW_MAX_PAGES; p++) {
		if (m_erased[p / 8] & (1u << (p % 8))) {
			continue;
		}
		int ret = flash_area_erase(fa, (off_t)p * FW_PAGE_SZ, FW_PAGE_SZ);

		if (ret) {
			return ret;
		}
		m_erased[p / 8] |= (1u << (p % 8));
	}
	return 0;
}

uint32_t fw_slot0_base(void)
{
	return (uint32_t)(FLASH_BASE + SLOT0_OFFSET);
}

size_t fw_slot0_size(void)
{
	return SLOT0_SIZE;
}

int fw_erase_slot(void)
{
	const struct flash_area *fa;
	int ret = flash_area_open(SLOT0_ID, &fa);

	if (ret) {
		return ret;
	}
	ret = flash_area_erase(fa, 0, SLOT0_SIZE);
	flash_area_close(fa);
	return ret;
}

int fw_write(uint32_t off, const uint8_t *data, size_t len)
{
	const struct flash_area *fa;
	int ret = flash_area_open(SLOT0_ID, &fa);

	if (ret) {
		return ret;
	}
	if (off + len > SLOT0_SIZE) {
		flash_area_close(fa);
		return -EFBIG;
	}
	if (m_incremental) {
		ret = ensure_erased(fa, off, len);
		if (ret) {
			flash_area_close(fa);
			return ret;
		}
	}
	ret = flash_area_write(fa, off, data, len);
	flash_area_close(fa);
	return ret;
}

int fw_read(uint32_t off, uint8_t *data, size_t len)
{
	const struct flash_area *fa;
	int ret = flash_area_open(SLOT0_ID, &fa);

	if (ret) {
		return ret;
	}
	ret = flash_area_read(fa, off, data, len);
	flash_area_close(fa);
	return ret;
}

int fw_slot_crc32(size_t len, uint32_t *crc)
{
	const struct flash_area *fa;
	int ret = flash_area_open(SLOT0_ID, &fa);

	if (ret) {
		return ret;
	}

	uint8_t buf[CRC_CHUNK];
	uint32_t acc = 0;
	size_t off = 0;

	while (off < len) {
		size_t n = MIN((size_t)CRC_CHUNK, len - off);

		ret = flash_area_read(fa, off, buf, n);
		if (ret) {
			break;
		}
		acc = crc32_ieee_update(acc, buf, n);
		off += n;
	}

	flash_area_close(fa);
	if (ret == 0) {
		*crc = acc;
	}
	return ret;
}

int meta_read(struct sfu_meta *meta)
{
	const struct flash_area *fa;
	int ret = flash_area_open(META_ID, &fa);

	if (ret) {
		return ret;
	}
	ret = flash_area_read(fa, 0, meta, sizeof(*meta));
	flash_area_close(fa);
	return ret;
}

int meta_write(const struct sfu_meta *meta)
{
	const struct flash_area *fa;
	int ret = flash_area_open(META_ID, &fa);

	if (ret) {
		return ret;
	}
	ret = flash_area_erase(fa, 0, META_SIZE);
	if (ret == 0) {
		/* STM32WL flash writes in 8-byte (doubleword) units, so the length
		 * must be a multiple of 8; sizeof(sfu_meta) is 36. Pad to the next
		 * doubleword in a zero-filled buffer (erased flash reads 0xFF, so the
		 * pad bytes are explicit). */
		uint8_t buf[ROUND_UP(sizeof(*meta), 8)] = {0};

		memcpy(buf, meta, sizeof(*meta));
		ret = flash_area_write(fa, 0, buf, sizeof(buf));
	}
	flash_area_close(fa);
	return ret;
}
