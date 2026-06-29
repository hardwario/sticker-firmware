/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_dfu_meta.h"
#include "app_config.h"
#include "app_log.h"

/* Shared bootloader contract: struct sfu_meta + SFU_META_MAGIC + NFC_KEY_LEN. */
#include <sticker/nfc_proto.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_dfu_meta, LOG_LEVEL_INF);

#define META_ID   FIXED_PARTITION_ID(sfu_meta_partition)
#define META_SIZE FIXED_PARTITION_SIZE(sfu_meta_partition)

int app_dfu_meta_provision(void)
{
	const struct flash_area *fa;
	struct sfu_meta meta;
	int ret = flash_area_open(META_ID, &fa);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("flash_area_open", ret);
		return ret;
	}

	ret = flash_area_read(fa, 0, &meta, sizeof(meta));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("flash_area_read", ret);
		goto out;
	}

	/* Idempotent: skip the erase/write (and its flash wear) when the stored
	 * record already carries this device's key + serial and marks the slot
	 * bootable. */
	if (meta.magic == SFU_META_MAGIC && meta.valid == SFU_META_MAGIC &&
	    meta.serial == g_app_config.serial_number &&
	    memcmp(meta.secret_key, g_app_config.secret_key, NFC_KEY_LEN) == 0) {
		LOG_DBG("sfu_meta already provisioned for serial %u", g_app_config.serial_number);
		goto out;
	}

	/* Keep the image descriptor (payload_len/crc32) from a committed record: the
	 * boot decision ignores it and a DFU overwrites the whole record anyway, but
	 * preserving it avoids lying about the image that is actually flashed. */
	struct sfu_meta rec = {
		.magic = SFU_META_MAGIC,
		.payload_len = (meta.magic == SFU_META_MAGIC) ? meta.payload_len : 0,
		.payload_crc32 = (meta.magic == SFU_META_MAGIC) ? meta.payload_crc32 : 0,
		/* The running app IS a committed, bootable image. Once magic is set the
		 * bootloader treats the record as authoritative, so valid MUST be MAGIC
		 * or the device would wedge in DFU instead of booting. Setting magic is
		 * also what makes the bootloader load this key (vs. unkeyed). */
		.valid = SFU_META_MAGIC,
		.serial = g_app_config.serial_number,
	};
	memcpy(rec.secret_key, g_app_config.secret_key, NFC_KEY_LEN);

	ret = flash_area_erase(fa, 0, META_SIZE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("flash_area_erase", ret);
		goto out;
	}

	/* STM32WL flash writes in 8-byte doublewords, so the length must be a
	 * multiple of 8 (sizeof(sfu_meta) is 36). Pad into a zero-filled buffer.
	 * Mirrors the bootloader's meta_write(). */
	uint8_t buf[ROUND_UP(sizeof(rec), 8)] = {0};

	memcpy(buf, &rec, sizeof(rec));
	ret = flash_area_write(fa, 0, buf, sizeof(buf));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("flash_area_write", ret);
		goto out;
	}

	LOG_INF("Provisioned sfu_meta (serial %u) for NFC DFU", g_app_config.serial_number);

out:
	flash_area_close(fa);
	return ret;
}
