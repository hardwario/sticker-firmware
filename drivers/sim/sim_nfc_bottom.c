/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator NFC phone link, host side: a UDP socket on localhost.
 * Built into the native simulator runner (host libc), called from
 * emul_st25dv.c. */

#include "sim_nfc_bottom.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int m_fd = -1;
static struct sockaddr_in m_client;
static bool m_client_known;

int sim_nfc_bottom_open(int port)
{
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = htons((uint16_t)port),
	};

	m_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (m_fd < 0) {
		return -errno;
	}
	if (bind(m_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
		int err = -errno;

		close(m_fd);
		m_fd = -1;
		return err;
	}
	(void)fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK);

	return port;
}

int sim_nfc_bottom_recv(void *buf, int max)
{
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	ssize_t n;

	if (m_fd < 0) {
		return -ENOTCONN;
	}

	n = recvfrom(m_fd, buf, max, 0, (struct sockaddr *)&from, &len);
	if (n < 0) {
		return (errno == EAGAIN || errno == ECONNREFUSED) ? 0 : -errno;
	}
	m_client = from;
	m_client_known = true;

	return (int)n;
}

int sim_nfc_bottom_reply(const void *buf, int len)
{
	if (m_fd < 0 || !m_client_known) {
		return -ENOTCONN;
	}

	/* A client that went away is fine: the answer is lost. */
	(void)sendto(m_fd, buf, len, 0, (struct sockaddr *)&m_client, sizeof(m_client));

	return 0;
}
