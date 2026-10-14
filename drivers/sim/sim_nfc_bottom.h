/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STICKER_DRIVERS_SIM_SIM_NFC_BOTTOM_H_
#define STICKER_DRIVERS_SIM_SIM_NFC_BOTTOM_H_

/* STICKER simulator NFC "phone" link, host side (sim_nfc_bottom.c): a UDP socket
 * on 127.0.0.1 that the host NFC client (scripts/sim/sim_nfc.py) talks to. Each
 * datagram is one RF operation of the phone; the emulated ST25DV
 * (emul_st25dv.c) answers the client that sent the latest one.
 *
 * Datagram = op(1) || data:
 *   'F' on(1)   RF field on/off                       -> 'S' status
 *   'S'         status                                -> 'S' status
 *   'X' msg     field on, MB_EN=1, put msg (RF_PUT)   -> 'R' reply, once the host
 *                                                        puts one in the mailbox,
 *               or 'E' code(1) when the mailbox is not usable
 *   'D'         RF clears MB_EN                       -> 'S' status
 * Status = 'S' field vcc mb_ctrl mb_len gpo_static gpo_dyn mb_mode i2c_sso
 */

#define SIM_NFC_DEFAULT_PORT 47801

#define SIM_NFC_OP_FIELD  'F'
#define SIM_NFC_OP_STATUS 'S'
#define SIM_NFC_OP_XFER   'X'
#define SIM_NFC_OP_MB_OFF 'D'
#define SIM_NFC_OP_REPLY  'R'
#define SIM_NFC_OP_ERROR  'E'

/* Error codes of an 'E' answer. */
#define SIM_NFC_ERR_NO_FTM 1 /* MB_MODE not set: the mailbox cannot be enabled */
#define SIM_NFC_ERR_BUSY   2 /* a previous RF message is still unread */
#define SIM_NFC_ERR_LEN    3 /* empty or longer than the 256 B mailbox */

/* Bind 127.0.0.1:port; returns the port or a negative errno. */
int sim_nfc_bottom_open(int port);
/* Non-blocking receive of one datagram; 0 when none. Remembers the sender. */
int sim_nfc_bottom_recv(void *buf, int max);
/* Send to the sender of the latest datagram (no-op before the first one). */
int sim_nfc_bottom_reply(const void *buf, int len);

#endif /* STICKER_DRIVERS_SIM_SIM_NFC_BOTTOM_H_ */
