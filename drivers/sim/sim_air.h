/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STICKER_DRIVERS_SIM_SIM_AIR_H_
#define STICKER_DRIVERS_SIM_SIM_AIR_H_

/* STICKER simulator "air": radio packets between the emulated SX1262
 * (emul_sx126x.c) and a peer on the host (scripts/sim/sim_air.py: LoRaWAN
 * network server, P2P hub), one UDP datagram per packet. Shared by the
 * embedded side and the host side (sim_air_bottom.c), so plain C types only.
 *
 * A datagram is the packed little-endian header below followed by `len`
 * payload bytes. The node sends PACKET for each transmission. The peer
 * answers each of them with a PACKET (a downlink, ref_seq = the node packet,
 * delay_us = start of its transmission after the end of that packet) or with
 * NONE, so the node knows when no answer is coming; it may also send PACKETs
 * on its own (delay_us = SIM_AIR_ASAP). HELLO goes both ways at start-up. */

#include <stdint.h>

#define SIM_AIR_MAGIC        0x4153 /* "SA" */
#define SIM_AIR_VERSION      1
#define SIM_AIR_DEFAULT_PEER "127.0.0.1:47800"
#define SIM_AIR_ASAP         UINT32_MAX
#define SIM_AIR_MAX_PAYLOAD  255

enum sim_air_type {
	SIM_AIR_HELLO = 1,
	SIM_AIR_PACKET = 2,
	SIM_AIR_NONE = 3,
};

enum sim_air_modem {
	SIM_AIR_GFSK = 0,
	SIM_AIR_LORA = 1,
};

struct sim_air_frame {
	uint16_t magic;
	uint8_t version;
	uint8_t type;
	uint32_t seq;      /* sender's packet number */
	uint32_t ref_seq;  /* reply: the node packet it answers */
	uint32_t delay_us; /* reply: TX start after the end of ref_seq */
	uint64_t time_us;  /* node packet: end of its transmission, node uptime */
	uint32_t freq_hz;
	uint32_t bw_hz;   /* LoRa bandwidth */
	uint32_t bitrate; /* GFSK */
	uint32_t fdev_hz; /* GFSK */
	uint8_t modem;
	uint8_t sf;
	uint8_t cr; /* 1..4 = 4/5..4/8 */
	uint8_t iq_inverted;
	uint8_t crc_on;
	uint8_t implicit_header;
	uint16_t preamble;  /* LoRa symbols, GFSK bits */
	uint16_t sync_word; /* LoRa sync word register, GFSK first two bytes */
	int8_t power_dbm;
	int8_t snr_db;
	int16_t rssi_dbm;
	uint16_t len;
	uint8_t data[SIM_AIR_MAX_PAYLOAD];
} __attribute__((packed));

#define SIM_AIR_HEADER_LEN ((int)(sizeof(struct sim_air_frame) - SIM_AIR_MAX_PAYLOAD))

/* Host side (sim_air_bottom.c). Return the local UDP port, or < 0. */
int sim_air_bottom_open(const char *peer);
int sim_air_bottom_send(const void *buf, int len);
/* Wait up to timeout_ms of host time (0 = poll); bytes read, 0 none, < 0 error. */
int sim_air_bottom_recv(void *buf, int max, int timeout_ms);

#endif /* STICKER_DRIVERS_SIM_SIM_AIR_H_ */
