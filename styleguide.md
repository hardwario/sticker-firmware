# STICKER Firmware — Coding Style Guide

## File structure

### Copyright header

Every source file starts with the Apache-2.0 copyright block:

```c
/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
```

### Include ordering

Includes are separated into groups by blank lines with section comments:

```c
#include "app_calibration.h"
#include "app_config.h"
#include "app_led.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stdint.h>
```

Order:
1. App headers (`#include "app_*.h"`) — alphabetical
2. Zephyr headers (`#include <zephyr/...>`) — preceded by `/* Zephyr includes */`
3. Third-party headers (e.g., LoRaMac) — preceded by descriptive comment
4. Standard C headers — preceded by `/* Standard includes */`

### LOG_MODULE_REGISTER

Always present in every `.c` file, immediately after includes, before `#define`:

```c
LOG_MODULE_REGISTER(app_battery, LOG_LEVEL_DBG);
```

- Always `LOG_LEVEL_DBG`
- Module name matches the file name without `.c`

### Macro definitions

After `LOG_MODULE_REGISTER`, before static variables:

```c
#define MAX_TIMEOUT_MSEC 10000
#define SENTINEL         ((int16_t)0x7FFF)
```

- Values right-aligned with tabs when multiple related macros
- Parentheses around complex expressions

### Static module variables

After macros, before static functions. Use `m_` prefix:

```c
static int m_count_ds18b20;
static const struct device *m_dev;
static atomic_t m_state;
```

### Global variables

Use `g_app_` prefix. Declared in header, defined in `.c`:

```c
struct app_sensor_data g_app_sensor_data;
K_MUTEX_DEFINE(g_app_sensor_data_lock);
```

### Function ordering

1. Static helper functions (private)
2. Static work/timer handlers
3. Static Zephyr kernel object definitions (`K_WORK_DEFINE`, `K_TIMER_DEFINE`)
4. Public functions (`app_<module>_*`)

---

## Naming conventions

### Functions

- **snake_case** exclusively
- Public: `app_<module>_<action>` — e.g., `app_battery_measure`, `app_hall_init`
- Private static: descriptive name without `app_` prefix — e.g., `poll`, `compose_calibration_payload`

### Variables

| Scope | Prefix | Example |
|-------|--------|---------|
| Static module | `m_` | `m_calibration_active`, `m_work_q` |
| Global exported | `g_app_` | `g_app_config`, `g_app_sensor_data` |
| Local | none | `ret`, `temperature`, `w1_ready` |

### Zephyr kernel objects

Follow the same prefix rules:

```c
K_MUTEX_DEFINE(m_hall_data_mutex);        /* static module */
K_MUTEX_DEFINE(g_app_sensor_data_lock);   /* global */
K_WORK_DEFINE(m_sensor_work, sensor_work_handler);
K_TIMER_DEFINE(m_sensor_timer, sensor_timer_handler, NULL);
static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
```

### Static const arrays

Use `m_` prefix:

```c
static const uint8_t m_cal_deveui[] = {0x02, 0x40, ...};
```

---

## Function signatures

### Return types

| Purpose | Return type | Example |
|---------|-------------|---------|
| Init / Read / Write | `int` (negative errno on error) | `int app_battery_init(void)` |
| Action / Fire-and-forget | `void` | `void app_lrw_send(void)` |
| Query / Predicate | `bool` | `bool app_calibration_is_active(void)` |

### Pointer parameters

Asterisk attached to the name, no space before `*`:

```c
int app_sht4x_read(float *temperature, float *humidity);
int app_hall_get_data(struct app_hall_data *data);
```

### Empty parameter list

Always `(void)`, never `()`:

```c
int app_calibration_init(void);
bool app_lrw_is_ready(void);
```

---

## Error handling

### Return values

Use negative errno constants:

```c
return -ENODEV;    /* device not ready */
return -EINVAL;    /* invalid argument */
return -EIO;       /* I/O error */
return -EALREADY;  /* already in state (often non-fatal) */
```

### Error logging

Use macros from `app_log.h`:

```c
LOG_ERR("Device not ready");
LOG_ERR_CALL_FAILED_INT("adc_channel_setup", ret);
LOG_ERR_CALL_FAILED_CTX_INT("device_init", "opt3001", ret);
```

- `LOG_ERR` — simple message
- `LOG_ERR_CALL_FAILED_INT(func, ret)` — function call failed with int error code
- `LOG_ERR_CALL_FAILED_CTX_INT(func, ctx, ret)` — with additional context string
- `LOG_WRN` — recoverable issues
- `LOG_INF` — important state changes

### Error accumulation pattern

When initializing multiple subsystems where each can fail independently:

```c
int res = 0;

ret = device_init(dev1);
if (ret) {
	LOG_ERR_CALL_FAILED_CTX_INT("device_init", "dev1", ret);
	res = res ? res : ret;
}

ret = device_init(dev2);
if (ret) {
	LOG_ERR_CALL_FAILED_CTX_INT("device_init", "dev2", ret);
	res = res ? res : ret;
}

return res;
```

### Goto pattern for cleanup

Used when a function needs to undo operations on error (e.g., PM suspend):

```c
int app_battery_measure(float *voltage)
{
	int ret;
	int res = 0;

#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_RESUME);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		return ret;
	}
#endif

	/* ... operations ... */

	if (ret) {
		res = ret;
		goto suspend;
	}

	/* ... more operations ... */

suspend:
#if defined(CONFIG_PM_DEVICE)
	ret = pm_device_action_run(m_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		return res ? res : ret;
	}
#endif

	return res;
}
```

---

## Conditional compilation

### Style

Always `#if defined()`, never `#ifdef`:

```c
#if defined(CONFIG_LORAWAN)
	app_lrw_send();
#endif /* defined(CONFIG_LORAWAN) */
```

### #endif comments

Every `#endif` has a comment matching the condition:

```c
#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */
```

### Common config guards

```c
#if defined(CONFIG_LORAWAN)
#if defined(CONFIG_WATCHDOG)
#if defined(CONFIG_PM_DEVICE)
#if defined(CONFIG_ADC)
#if defined(CONFIG_SHT4X)
#if defined(CONFIG_SHELL)
```

---

## Device initialization patterns

### Deferred init (sensors not started at boot)

```c
const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sht40));

ret = device_init(dev);
if (ret && ret != -EALREADY) {
	LOG_ERR_CALL_FAILED_CTX_INT("device_init", "sht40", ret);
	return ret;
}
```

- `-EALREADY` is not an error (device already initialized)

### Device ready check (drivers started at boot)

```c
const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

if (!device_is_ready(dev)) {
	LOG_ERR("Device not ready");
	return -ENODEV;
}
```

### GPIO device check

```c
if (!gpio_is_ready_dt(&m_hall_left)) {
	LOG_ERR("Hall left GPIO device not ready");
	return -ENODEV;
}
```

---

## PM device pattern

Resume before peripheral access, suspend after:

```c
#if defined(CONFIG_PM_DEVICE)
ret = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
if (ret && ret != -EALREADY) {
	LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
	return ret;
}
#endif

/* ... peripheral operations ... */

#if defined(CONFIG_PM_DEVICE)
ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
if (ret && ret != -EALREADY) {
	LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
}
#endif
```

- Always check `ret != -EALREADY`
- Suspend even on operation failure (use goto pattern)

---

## Watchdog feed

```c
#if defined(CONFIG_WATCHDOG)
app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */
```

- Place between long-running operations
- Always inside `#if defined(CONFIG_WATCHDOG)` guard

---

## Struct initialization

### Simple struct (single line)

```c
struct app_led_blink_req req = {
	.color = APP_LED_CHANNEL_Y, .duration = 100, .space = 100, .repetitions = 5};
```

### Complex struct (multi-line)

```c
struct app_led_play_req req = {
	.commands = {{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
		     {.type = APP_LED_CMD_DELAY, .duration = 500},
		     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
		     {.type = APP_LED_CMD_END}},
	.repetitions = 1};
```

- Continuation lines aligned with first array element
- Closing `};` on last member line

### Const data struct

```c
static const struct adc_channel_cfg m_channel_cfg = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_MAX,
	.channel_id = ADC_CHANNEL_BATT,
	.differential = 0,
};
```

- Trailing comma after last member
- Closing `};` on separate line

---

## Function body structure

1. Variable declarations (`int ret;`)
2. Early guard checks (`if (!condition) return`)
3. Blank line
4. PM resume (if applicable)
5. Blank line
6. Main logic with blank lines between sections
7. PM suspend (if applicable)
8. Return

---

## Header file structure

```c
/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MODULE_H_
#define APP_MODULE_H_

#include <stdbool.h>    /* only standard type headers if needed */

#ifdef __cplusplus
extern "C" {
#endif

/* defines, enums, structs */

int app_module_init(void);
int app_module_read(float *value);
bool app_module_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MODULE_H_ */
```

- Include guard: `APP_<MODULE>_H_` with trailing underscore
- `extern "C"` guards always present
- Only standard type includes (`stdbool.h`, `stdint.h`, `stddef.h`) — no app or Zephyr includes
- `#endif` comment matches guard name

---

## Formatting

### Indentation

- **Tab character** exclusively (no spaces for indentation)
- Tab width: project convention (typically 8 or editor-dependent)

### Line length

- No strict limit enforced
- Target ~90-100 characters
- Wrap long function calls at parameter boundaries:

```c
ret = adc_raw_to_millivolts(adc_ref_internal(m_dev), m_channel_cfg.gain, seq.resolution,
			    &voltage_);
```

### Blank lines

- One blank line between logical sections
- One blank line before `if`/`switch`/`for` blocks (when separating concerns)
- No blank line after opening `{` or before closing `}`

### sizeof

Always parenthesized: `sizeof(variable)`, `sizeof(type)`
