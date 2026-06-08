/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_writer.h"

#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

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

#define CRC_CHUNK 256

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
	ret = flash_area_write(fa, off, data, len);
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
	ret = flash_area_erase(fa, 0, flash_area_get_size(fa));
	if (ret == 0) {
		ret = flash_area_write(fa, 0, meta, sizeof(*meta));
	}
	flash_area_close(fa);
	return ret;
}
