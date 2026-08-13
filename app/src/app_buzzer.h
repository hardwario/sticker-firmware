/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BUZZER_H_
#define APP_BUZZER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* #338 wire melody ids (kept in sync with MELODY_TABLE in app_buzzer.c and the
 * doc comment on Command.BuzzerPlay.kind in app_config.proto): 0 = no-op,
 * 1-3 assigned, 4-15 reserved for future melodies. Adding a new melody only
 * touches app_buzzer.c (a new PATTERN_* + MELODY_TABLE row) and this list of
 * defines — app_config.proto / app_cmd.c pass the raw id through unchanged. */
#define APP_BUZZER_KIND_INFO    1
#define APP_BUZZER_KIND_WARNING 2
#define APP_BUZZER_KIND_ALARM   3

int app_buzzer_init(void);
/* on=false stops immediately, aborting a beep/melody currently mid-playback
 * on the buzzer thread instead of waiting for it to run out. */
int app_buzzer_set(bool on);
int app_buzzer_on(uint32_t duration_ms);

/* Play a melody by id once. kind==0 ("nothing") and an unmapped id (4-15) are
 * both accepted calls: 0 succeeds as a no-op, an unmapped id returns -ENOENT
 * ("this melody doesn't exist") — app_buzzer.c is the only place that decides
 * which ids are valid. */
int app_buzzer_play(uint8_t kind);

/* Same as app_buzzer_play(), but if repeat_s is non-zero (1-999) the melody
 * replays that many seconds after it finishes, indefinitely, until
 * app_buzzer_set(false) stops it. Omit this and call app_buzzer_play() for
 * the common one-shot case. */
int app_buzzer_play_repeating(uint8_t kind, uint16_t repeat_s);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUZZER_H_ */
