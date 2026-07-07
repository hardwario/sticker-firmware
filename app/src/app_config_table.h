/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONFIG_TABLE_H_
#define APP_CONFIG_TABLE_H_

/* Generic table-driven config apply/fill (#262).
 *
 * configen emits one rodata `struct cfg_field_desc[]` per proto submessage group
 * (in app_config_ingest.c) instead of per-field inline code; the three walkers
 * below are hand-written once and shared by every group, replacing ~40-80 B of
 * generated text per field with ~16 B of rodata. No wire-format change: the
 * descriptors encode exactly the validation/mapping the generated code did.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_cmd.h" /* enum app_cmd_transport (per-field write-transport gating, M-3) */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-field handling kind (how cfg_apply/cfg_fill interpret the value). */
enum cfg_kind {
	CFG_KIND_BOOL = 0, /* 1-byte bool; copied verbatim, no validation */
	CFG_KIND_PLAIN,    /* scalar copied as-is (no range check), e.g. a bitmask */
	CFG_KIND_RANGED,   /* scalar validated against [min, max] (enums use [0, max]) */
	CFG_KIND_BYTES,    /* fixed-length byte blob; memcpy of `size` bytes */
};

/* Descriptor flag bits. */
#define CFG_F_NO_WR_LRW (1U << 0) /* reject a SetParam write over LoRaWAN (M-3) */
#define CFG_F_NO_WR_NFC (1U << 1) /* reject a SetParam write over NFC (M-3) */
#define CFG_F_DUMP      (1U << 2) /* readable over the air -> emitted by cfg_fill */
#define CFG_F_ZERO_OK   (1U << 3) /* RANGED: value 0 accepted regardless of [min, max] */
#define CFG_F_OMIT_ZERO (1U << 4) /* BYTES: an all-zero slot is omitted from a dump */

/* One config field: maps a proto submessage field <-> struct app_config member.
 * `has_off`/`src_off` are offsets into the proto submessage struct; `dst_off` is
 * the offset into struct app_config. Emitted as rodata by configen. */
struct cfg_field_desc {
	uint16_t tag;     /* proto field id within the group */
	uint8_t kind;     /* enum cfg_kind */
	uint8_t size;     /* scalar width (1/2/4) or byte-blob length */
	uint8_t flags;    /* CFG_F_* */
	uint16_t has_off; /* offsetof(proto submessage, has_<field>) */
	uint16_t src_off; /* offsetof(proto submessage, <field>) */
	uint16_t dst_off; /* offsetof(struct app_config, <member>) */
	int32_t min;      /* RANGED lower bound */
	int32_t max;      /* RANGED upper bound (enum: highest valid value) */
};

/* Apply the present fields of proto submessage `src` into flat config `config`,
 * validating per descriptor. Mirrors the old generated apply_<group>(): invalid
 * or non-writable fields are skipped, the first offender's tag is recorded in
 * *fault_field (if non-NULL). Returns 0 if all present fields were valid and
 * writable over `tp`; -EACCES if a field was not writable over `tp`; -EINVAL on
 * an out-of-range value (first offender wins, -EACCES taking precedence like the
 * generated code, since the transport gate is checked before the value). */
int cfg_apply(const struct cfg_field_desc *tbl, size_t n, enum app_cmd_transport tp,
	      const void *src, void *config, uint32_t *fault_field);

/* Populate proto submessage `dst` with the dumpable fields whose tag is in
 * ids[0..n_ids), reading from flat `config` and setting each has_* flag. Mirrors
 * the old generated fill_<group>(): non-dumpable fields and all-zero omit-if-zero
 * byte slots are skipped. */
void cfg_fill(const struct cfg_field_desc *tbl, size_t n, void *dst, const void *config,
	      const uint32_t *ids, size_t n_ids);

/* True when dump field `tag` is an omit-if-zero byte slot that is currently
 * all-zero (so cfg_fill would omit it). Used by the get_config paging loop to
 * keep the page budget exact. Returns false for any other tag. */
bool cfg_slot_empty(const struct cfg_field_desc *tbl, size_t n, const void *config, uint32_t tag);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_TABLE_H_ */
