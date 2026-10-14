/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator "air", host side: a UDP socket to the peer. Built into
 * the native simulator runner (host libc), called from emul_sx126x.c. */

#include "sim_air.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int m_fd = -1;
static struct sockaddr_in m_peer;

int sim_air_bottom_open(const char *peer)
{
	char host[64];
	const char *colon = strrchr(peer, ':');
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	socklen_t len = sizeof(local);

	if (colon == NULL || (size_t)(colon - peer) >= sizeof(host)) {
		fprintf(stderr, "sim_air: bad peer address '%s' (host:port)\n", peer);
		return -EINVAL;
	}
	memcpy(host, peer, colon - peer);
	host[colon - peer] = '\0';

	memset(&m_peer, 0, sizeof(m_peer));
	m_peer.sin_family = AF_INET;
	m_peer.sin_port = htons((uint16_t)atoi(colon + 1));
	if (inet_pton(AF_INET, host, &m_peer.sin_addr) != 1) {
		fprintf(stderr, "sim_air: bad peer host '%s'\n", host);
		return -EINVAL;
	}

	m_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_fd < 0) {
		return -errno;
	}
	if (bind(m_fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    getsockname(m_fd, (struct sockaddr *)&local, &len) < 0) {
		int err = -errno;

		close(m_fd);
		m_fd = -1;
		return err;
	}
	(void)fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK);

	return ntohs(local.sin_port);
}

int sim_air_bottom_send(const void *buf, int len)
{
	if (m_fd < 0) {
		return -ENOTCONN;
	}

	/* No peer listening is fine: the packet is lost in the air. */
	(void)sendto(m_fd, buf, len, 0, (struct sockaddr *)&m_peer, sizeof(m_peer));

	return 0;
}

int sim_air_bottom_recv(void *buf, int max, int timeout_ms)
{
	struct pollfd pfd = {.fd = m_fd, .events = POLLIN};
	ssize_t n;

	if (m_fd < 0) {
		return -ENOTCONN;
	}
	if (poll(&pfd, 1, timeout_ms) <= 0) {
		return 0;
	}

	n = recv(m_fd, buf, max, 0);
	if (n < 0) {
		return (errno == EAGAIN || errno == ECONNREFUSED) ? 0 : -errno;
	}

	return (int)n;
}
