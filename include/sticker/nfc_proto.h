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

#define SFU_HEADER_LEN 32

/*
 * Authentication: symmetric AES-CCM with the per-device secret_key (same key
 * the config-ingest path uses). The phone encrypts each frame; the bootloader
 * decrypts with the stored key. A device whose stored key is all-zero accepts
 * plaintext frames unconditionally (factory bootstrap). The nonce_counter is
 * NOT used for replay protection here (per design); a per-session value sent
 * in CMD_START diversifies the nonce instead.
 */
#define NFC_KEY_LEN       16
#define NFC_CCM_TAG_LEN   8
#define NFC_CCM_NONCE_LEN 13 /* serial(4) | session(4) | seq(4) | 0 */

/* Plaintext bytes per DATA frame (frame data = ciphertext + tag). */
#define NFC_MAX_PLAINTEXT (NFC_MAX_DATA - NFC_CCM_TAG_LEN) /* 232 */

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

/* Validity + key metadata. Written by the bootloader after a successful
 * update, and refreshed by the application on every boot so the bootloader
 * always has the current secret_key (the app provisions it from its config).
 * A blank/erased record or an all-zero key means "unkeyed" (factory). */
#define SFU_META_MAGIC 0x53464D31u /* "SFM1" */

struct sfu_meta {
	uint32_t magic;        /* SFU_META_MAGIC when valid */
	uint32_t payload_len;
	uint32_t payload_crc32;
	uint32_t valid;        /* SFU_META_MAGIC -> bootable */
	uint8_t secret_key[NFC_KEY_LEN];
	uint32_t serial;       /* device serial, part of the CCM nonce */
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

/* ---- FTM mailbox binding (doc §4) ----------------------------------- */
/*
 * The ST25DV Fast-Transfer-Mode mailbox is a single 256-byte volatile RAM
 * buffer, shared half-duplex between the RF (phone) and I2C (MCU) sides:
 *   - phone writes a request via the RF "Write Message" command (arms RF_PUT);
 *   - MCU polls MB_CTRL_Dyn.RF_PUT, reads MB_LEN_Dyn + the buffer, processes it;
 *   - MCU writes the response into the same buffer (arms HOST_PUT);
 *   - phone reads it via "Read Msg Length" + "Read Message".
 * No EEPROM wear, no 5 ms page programming — a whole frame moves in one I2C
 * transaction. The register addresses and RF command opcodes live MCU-side in
 * boot/src/st25dv_mb.c and phone-side in the flasher; only the logical frame
 * sizes are shared here.
 */
#define NFC_MB_RAM_SIZE 256 /* ST25DV04K Fast-Transfer mailbox RAM */

/* A request frame ([type][seq][len] + up to NFC_MAX_DATA) must fit the buffer
 * (4 + 240 = 244 <= 256). */
#define NFC_MB_REQ_LEN NFC_MB_RAM_SIZE
/* A response frame is just [status][ctx_lo][ctx_hi] (+ optional detail). */
#define NFC_MB_RSP_LEN 16

#ifdef __cplusplus
}
#endif

#endif /* STICKER_NFC_PROTO_H */
