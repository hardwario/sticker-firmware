/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CALIBRATION_H_
#define APP_CALIBRATION_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timing - Activation */
#define CALIBRATION_ACTIVATION_WINDOW_S (30 * 60)  /* 30 minutes from boot */
#define CALIBRATION_HALL_TOLERANCE_S    5          /* 5 seconds between both halls */
#define CALIBRATION_HALL_HOLD_S         2          /* 2 seconds hold required */

/* Timing - Operation */
#define CALIBRATION_SEND_INTERVAL_S     30            /* LoRaWAN send interval */
#define CALIBRATION_RTT_INTERVAL_S      1             /* RTT output interval */
#define CALIBRATION_MAX_DURATION_S      (2 * 60 * 60) /* 2 hours max (Hall-activated only) */

/* LoRaWAN */
#define CALIBRATION_DATARATE            5  /* DR5 */
#define CALIBRATION_ADR_ENABLED         false

/* Fixed ABP Credentials for calibration mode */
/* DevEUI: 4b823d16dd16c11c, JoinEUI: 0000000000000000 (for reference) */
#define CALIBRATION_DEVADDR             0x004f2a32
#define CALIBRATION_NWKSKEY                                                                        \
	{                                                                                          \
		0x83, 0xc3, 0x08, 0x7c, 0xca, 0xe4, 0x0c, 0xac, 0x26, 0xac, 0xd3, 0x60, 0xb9, 0xe3, \
			0xae, 0xbc                                                                     \
	}
#define CALIBRATION_APPSKEY                                                                        \
	{                                                                                          \
		0x72, 0x1b, 0xcb, 0x16, 0xca, 0xb4, 0x7b, 0x1d, 0x3c, 0x3f, 0x74, 0x59, 0x16, 0x7f, \
			0xb8, 0x98                                                                     \
	}

/**
 * @brief Initialize calibration mode
 *
 * If config calibration is enabled, enters calibration mode directly.
 * Otherwise monitors Hall sensors for activation gesture during startup window.
 *
 * @return 0 on success (calibration not active or completed), negative errno on error
 */
int app_calibration_init(void);

/**
 * @brief Check if calibration mode is currently active
 *
 * @return true if in calibration mode
 */
bool app_calibration_is_active(void);

/**
 * @brief Check Hall sensors for calibration activation gesture
 *
 * Must be called during startup window (first 30 minutes).
 * Detects both Hall sensors held for 2 seconds within 5 second tolerance.
 *
 * @return true if calibration gesture detected
 */
bool app_calibration_check_hall_gesture(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CALIBRATION_H_ */
