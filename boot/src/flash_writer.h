/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * slot0 erase/write and sfu_meta persistence for the NFC bootloader.
 * WIP scaffold — not yet HW-tested.
 */

#ifndef STICKER_BOOT_FLASH_WRITER_H
#define STICKER_BOOT_FLASH_WRITER_H

#include <sticker/nfc_proto.h>

#include <stddef.h>
#include <stdint.h>

/* Absolute address the application is linked at (slot0 base). */
uint32_t fw_slot0_base(void);

/* Usable application slot size in bytes. */
size_t fw_slot0_size(void);

/* Erase the whole application slot in preparation for a new image. */
int fw_erase_slot(void);

/* Write `len` bytes at `off` within the application slot. */
int fw_write(uint32_t off, const uint8_t *data, size_t len);

/* Read `len` bytes at `off` from the application slot. */
int fw_read(uint32_t off, uint8_t *data, size_t len);

/* CRC-32/IEEE over the first `len` bytes already written to the slot. */
int fw_slot_crc32(size_t len, uint32_t *crc);

/* Read / write the validity metadata record. */
int meta_read(struct sfu_meta *meta);
int meta_write(const struct sfu_meta *meta);

#endif /* STICKER_BOOT_FLASH_WRITER_H */
