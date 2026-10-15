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

/* ---- ST25DV Fast-Transfer-Mode mailbox (#313) --------------------------------
 * The dual-port mailbox: RF and I2C exchange 256 B messages while the field is
 * on. The RF helpers below stand in for the phone; the I2C side is app_nfc.c's
 * mb_* functions going through the normal transfer path. */

/* MB_MODE authorised (static bit set at boot), MB_CTRL_Dyn, GPO static reg — for
 * asserting boot configuration. */
bool st25dv_emul_mb_mode(void);
uint8_t st25dv_emul_mb_ctrl(void);
uint8_t st25dv_emul_gpo_reg(void);

/* Make the password write (nfc_present_password) and the static-config writes it
 * guards fail, so app_nfc.c cannot authorise FTM — models a unit whose MB_MODE
 * cannot be set (a production defect the tester must catch). */
void st25dv_emul_set_pwd_fail(bool fail);

/* RF side (the phone): enable/disable the mailbox (MB_EN, RF-writable), put a
 * message the I2C host will read (sets RF_PUT_MSG), and read back the reply the
 * I2C host wrote (clears HOST_PUT_MSG). _rf_read_message returns -EAGAIN when no
 * host reply is waiting, -EMSGSIZE if it will not fit `cap`. */
void st25dv_emul_rf_set_mb_en(bool on);
int st25dv_emul_rf_put_message(const uint8_t *msg, size_t len);
int st25dv_emul_rf_read_message(uint8_t *out, size_t cap, size_t *len);

#endif /* EMUL_ST25DV_H_ */
