/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config_table.h"

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Scalar load/store go through memcpy so the byte offsets in the descriptor need
 * no alignment guarantees and no type punning (GCC folds a constant-size memcpy
 * into a single load/store). The target is little-endian, so a `size`-byte load
 * zero-extends into the low bytes of the returned uint32. */
static uint32_t load_scalar(const void *p, uint8_t size)
{
	uint32_t v = 0;

	memcpy(&v, p, size);
	return v;
}

static void store_scalar(void *p, uint8_t size, uint32_t v)
{
	memcpy(p, &v, size);
}

static bool all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return false;
		}
	}
	return true;
}

static bool requested(const uint32_t *ids, size_t n, uint32_t tag)
{
	for (size_t i = 0; i < n; i++) {
		if (ids[i] == tag) {
			return true;
		}
	}
	return false;
}

int cfg_apply(const struct cfg_field_desc *tbl, size_t n, enum app_cmd_transport tp,
	      const void *src, void *config, uint32_t *fault_field)
{
	const uint8_t *sbase = src;
	uint8_t *dbase = config;
	int ret = 0;

	if (fault_field) {
		*fault_field = 0;
	}

	for (size_t i = 0; i < n; i++) {
		const struct cfg_field_desc *d = &tbl[i];

		/* Field absent from this SetParam (nanopb has_* is a leading bool). */
		if (!sbase[d->has_off]) {
			continue;
		}

		/* M-3: reject a write over a transport not in the field's writable
		 * list. Checked before the value (mirrors the generated order), so a
		 * not-writable field reports -EACCES rather than a range error. */
		if ((tp == APP_CMD_TRANSPORT_LRW && (d->flags & CFG_F_NO_WR_LRW)) ||
		    (tp == APP_CMD_TRANSPORT_NFC && (d->flags & CFG_F_NO_WR_NFC))) {
			if (fault_field && *fault_field == 0) {
				*fault_field = d->tag;
			}
			if (ret == 0) {
				ret = -EACCES;
			}
			continue;
		}

		switch (d->kind) {
		case CFG_KIND_BOOL:
			store_scalar(dbase + d->dst_off, 1,
				     load_scalar(sbase + d->src_off, 1) ? 1 : 0);
			break;
		case CFG_KIND_BYTES:
			/* Native fixed_length bytes: nanopb decodes exactly `size` bytes. */
			memcpy(dbase + d->dst_off, sbase + d->src_off, d->size);
			break;
		case CFG_KIND_PLAIN:
			store_scalar(dbase + d->dst_off, d->size,
				     load_scalar(sbase + d->src_off, 4));
			break;
		case CFG_KIND_RANGED: {
			/* proto scalars are uint32; read 4 bytes and range-check as
			 * int32, matching the generated `int val = src->field;`. */
			int32_t val = (int32_t)load_scalar(sbase + d->src_off, 4);

			if (((d->flags & CFG_F_ZERO_OK) && val == 0) ||
			    (val >= d->min && val <= d->max)) {
				store_scalar(dbase + d->dst_off, d->size, (uint32_t)val);
			} else {
				if (fault_field && *fault_field == 0) {
					*fault_field = d->tag;
				}
				if (ret == 0) {
					ret = -EINVAL;
				}
			}
			break;
		}
		}
	}

	return ret;
}

void cfg_fill(const struct cfg_field_desc *tbl, size_t n, void *dst, const void *config,
	      const uint32_t *ids, size_t n_ids)
{
	uint8_t *dbase = dst;
	const uint8_t *cbase = config;

	for (size_t i = 0; i < n; i++) {
		const struct cfg_field_desc *d = &tbl[i];

		if (!(d->flags & CFG_F_DUMP) || !requested(ids, n_ids, d->tag)) {
			continue;
		}

		if (d->kind == CFG_KIND_BYTES) {
			if ((d->flags & CFG_F_OMIT_ZERO) && all_zero(cbase + d->dst_off, d->size)) {
				continue;
			}
			dbase[d->has_off] = true;
			memcpy(dbase + d->src_off, cbase + d->dst_off, d->size);
		} else if (d->kind == CFG_KIND_BOOL) {
			dbase[d->has_off] = true;
			store_scalar(dbase + d->src_off, 1, load_scalar(cbase + d->dst_off, 1));
		} else {
			/* Scalar: widen the config value into the uint32/enum proto field. */
			dbase[d->has_off] = true;
			store_scalar(dbase + d->src_off, 4,
				     load_scalar(cbase + d->dst_off, d->size));
		}
	}
}

bool cfg_slot_empty(const struct cfg_field_desc *tbl, size_t n, const void *config, uint32_t tag)
{
	const uint8_t *cbase = config;

	for (size_t i = 0; i < n; i++) {
		if (tbl[i].tag == tag && (tbl[i].flags & CFG_F_OMIT_ZERO)) {
			return all_zero(cbase + tbl[i].dst_off, tbl[i].size);
		}
	}

	return false;
}
