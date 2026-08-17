/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-facing control surface for the ST25DV i2c_emul model (emul_st25dv.c).
 * Lets a ztest drive the emulated tag's EEPROM/register state directly and
 * inject the specific failure modes app_nfc.c's fixes (M3/M15 arm-before-write,
 * vendor-transport clm gating) need to be exercised against.
 */
#ifndef EMUL_ST25DV_H_
#define EMUL_ST25DV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST25DV_EMUL_MEM_SIZE 512

/* Reset the whole model to power-on defaults: EEPROM zeroed, field off,
 * no pending RF-write flag, no injected failures. */
void st25dv_emul_reset(void);

/* Direct EEPROM peek/poke, bypassing the I2C transfer path entirely — for
 * asserting what app_nfc.c actually wrote, or seeding a "tag already holds
 * this content" precondition without going through write_mem() first. */
void st25dv_emul_mem_get(uint8_t *out, size_t offset, size_t len);
void st25dv_emul_mem_set(const uint8_t *data, size_t offset, size_t len);

/* Simulate the RF field being up (nfc_wait_field_off() spins) or down
 * (default; I2C proceeds immediately). */
void st25dv_emul_set_field_on(bool on);

/* Make the NEXT `count` I2C writes to the EEPROM memory range (reg < 0x2000,
 * i.e. write_mem()'s actual tag-content writes — not register pokes like the
 * GPO setup at init) fail with -EIO, simulating an RF collision / I2C error
 * mid-write. Register writes and reads are never affected. */
void st25dv_emul_inject_write_fail(int count);

#endif /* EMUL_ST25DV_H_ */
