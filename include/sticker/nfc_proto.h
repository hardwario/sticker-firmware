/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NFC firmware update protocol — shared contract between the application,
 * the NFC bootloader and the tools/nfc-flasher app.
 * Specification: doc/nfc-update-protocol.md. Keep the Dart mirror
 * (tools/nfc-flasher/lib/src/protocol.dart) in sync with this file.
 */

#ifndef STICKER_NFC_PROTO_H
#define STICKER_NFC_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Update image format (.sfu) ------------------------------------- */

#define SFU_MAGIC0 0x53 /* 'S' */
#define SFU_MAGIC1 0x4E /* 'N' */
#define SFU_MAGIC2 0x46 /* 'F' */
#define SFU_MAGIC3 0x55 /* 'U' */

#define SFU_HDR_VERSION 1

#define SFU_FLAG_SIGNED      0x0001
#define SFU_FLAG_CRC_PRESENT 0x0002

#define SFU_HEADER_LEN    32
#define SFU_SIGNATURE_LEN 64 /* Ed25519 */
#define SFU_PREAMBLE_LEN  (SFU_HEADER_LEN + SFU_SIGNATURE_LEN) /* 96 */

/* 32-byte image header, little-endian on the wire. */
struct sfu_header {
	uint8_t magic[4];      /* "SNFU" */
	uint16_t hdr_version;  /* SFU_HDR_VERSION */
	uint16_t flags;        /* SFU_FLAG_* */
	uint32_t fw_version;   /* (major<<24)|(minor<<16)|(patch<<8)|build_type */
	uint32_t payload_len;  /* firmware byte count (<= slot0 size) */
	uint32_t load_addr;    /* target flash base, e.g. 0x08008000 */
	uint32_t payload_crc32;/* CRC-32/IEEE of payload */
	uint8_t reserved[8];
} __attribute__((packed));

/* Validity metadata committed to flash after a successful update. */
#define SFU_META_MAGIC 0x53464D31u /* "SFM1" */

struct sfu_meta {
	uint32_t magic;        /* SFU_META_MAGIC when valid */
	uint32_t payload_len;
	uint32_t payload_crc32;
	uint32_t valid;        /* SFU_META_MAGIC -> bootable */
};

/* ---- Logical frame layer -------------------------------------------- */

/* Max firmware payload bytes per DATA frame. */
#define NFC_MAX_DATA 240

/* Request header: [type][seq_lo][seq_hi][len][data...] */
#define NFC_REQ_HDR_LEN 4

/* Commands (phone -> MCU). */
#define NFC_CMD_START  0x01
#define NFC_CMD_DATA   0x02
#define NFC_CMD_FINISH 0x03
#define NFC_CMD_ABORT  0x04
#define NFC_CMD_PING   0x05

/* Status codes (MCU -> phone). */
#define NFC_ST_READY      0x10
#define NFC_ST_ACK        0x11
#define NFC_ST_RETRY      0x12
#define NFC_ST_OK         0x13
#define NFC_ST_ERR_MAGIC  0x20
#define NFC_ST_ERR_SIZE   0x21
#define NFC_ST_ERR_FLASH  0x22
#define NFC_ST_ERR_VERIFY 0x23
#define NFC_ST_ERR_STATE  0x24

#define NFC_ST_IS_ERROR(s) ((s) >= 0x20)

/* ---- EEPROM software-mailbox binding (baseline, doc §4) ------------- */
/* ST25DV ISO15693 blocks are 4 bytes; offsets below are byte offsets.   */

#define NFC_MB_PH_FLAG_OFF 0x000 /* phone->MCU request-ready flag (1 byte) */
#define NFC_MB_REQ_OFF     0x004 /* request frame, up to 260 bytes        */
#define NFC_MB_REQ_LEN     260
#define NFC_MB_MC_FLAG_OFF 0x108 /* MCU->phone response-ready flag         */
#define NFC_MB_RSP_OFF     0x10C /* response frame, up to 16 bytes         */
#define NFC_MB_RSP_LEN     16

#define NFC_MB_FLAG_SET   1
#define NFC_MB_FLAG_CLEAR 0

#ifdef __cplusplus
}
#endif

#endif /* STICKER_NFC_PROTO_H */
