/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_alarm.h"
#include "app_analog.h"
#include "app_battery.h"
#include "app_buzzer.h"
#include "app_config.h"
#include "app_counters.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"
#include "app_machine_probe.h"
#include "app_mpl3115a2.h"
#include "app_opt3001.h"
#include "app_pyq1648.h"
#include "app_sensor.h"
#include "app_sensor_types.h"
#include "app_sht4x.h"
#include "app_w1.h"
#include "app_w1_slots.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_sensor, LOG_LEVEL_DBG);

/* I2C bus recovery: if every I2C sensor read in a sweep fails for this many
 * consecutive sweeps, the bus is likely wedged (a slave holding SDA low after a
 * brown-out/EMI glitch). i2c_recover_bus() bit-bangs 9 clocks to free it. */
#define I2C_RECOVER_FAIL_THRESHOLD 3
static const struct device *const m_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static int m_i2c_fail_streak;

/* #485 S2: consecutive failed reads per onboard I2C sensor (app_sensor_miss_hold).
 * Guarded by g_app_sensor_data_lock, like m_i2c_fail_streak. */
enum mb_src {
	MB_SRC_ACCEL,
	MB_SRC_SHT,
	MB_SRC_LIGHT,
	MB_SRC_BARO,
	MB_SRC_COUNT
};
static uint8_t m_mb_miss[MB_SRC_COUNT];

BUILD_ASSERT(APP_SENSOR_CH_MOTHERBOARD_COUNT <= 32, "motherboard valid mask is a uint32");
BUILD_ASSERT(APP_SENSOR_W1_CH_MAX <= 32, "1-Wire valid mask is a uint32");

/* Every channel starts absent (NaN); mb_counters_init() zeroes the counter
 * channels in app_sensor_init(). */
struct app_sensor_data g_app_sensor_data = {
	.mb =
		{
			.v =
				{
					[0 ... APP_SENSOR_CH_MOTHERBOARD_COUNT - 1] = {.f = NAN},
				},
		},
	.w1 =
		{
			[0 ... APP_W1_SLOT_COUNT - 1] =
				{
					.v =
						{
							[0 ... APP_SENSOR_W1_CH_MAX -
							 1] = {.f = NAN},
						},
				},
		},
};

K_MUTEX_DEFINE(g_app_sensor_data_lock);

/* The motherboard counter channels live in g_app_sensor_data between samples:
 * PIR / accel events increment them directly, app_sensor_sample() refreshes the
 * hall / input ones from their drivers. */
static void mb_counters_init(void)
{
	const struct app_sensor_type *t = app_sensor_type_get(APP_SENSOR_TYPE_MOTHERBOARD);

	for (uint8_t ch = 0; ch < t->channel_count; ch++) {
		if (t->channels[ch].flags & APP_SENSOR_F_COUNTER) {
			g_app_sensor_data.mb.v[ch].u = 0;
		}
	}
}

int app_sensor_read_channels(const struct device *dev, const enum sensor_channel *chans, float *out,
			     size_t n)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	int ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_sample_fetch", ret);
		return ret;
	}

	for (size_t i = 0; i < n; i++) {
		struct sensor_value val;

		ret = sensor_channel_get(dev, chans[i], &val);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
			return ret;
		}

		out[i] = sensor_value_to_float(&val);
	}

	return 0;
}

static void mb_put(struct app_sensor_mb *mb, uint8_t ch, float value)
{
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb->v, &mb->valid, ch, value);
}

static void mb_put_u(struct app_sensor_mb *mb, uint8_t ch, uint32_t value)
{
	mb->v[ch].u = value;
	mb->valid |= BIT(ch);
}

/* HW high-water mark 656 B (CONFIG_INIT_STACKS, 2026-10-09); >= 2x margin. */
static K_THREAD_STACK_DEFINE(m_sensor_work_stack, 1536);
static struct k_work_q m_sensor_work_q;

static void sensor_work_handler(struct k_work *work)
{
	app_sensor_sample();

	/* Back up the pulse totalizers at the sample cadence (interval_sample),
	 * which is typically far tighter than the report/send cadence. This timer
	 * is armed at boot and runs independently of the LoRaWAN join state, so
	 * counters persist even on a device that never joins. Dirty-flagged, so a
	 * no-op when nothing changed (no flash wear). When interval_sample == 0
	 * this handler does not run; app_report saves at the report cadence
	 * instead (see app_report.c). */
	(void)app_counters_save(false);
}

static K_WORK_DEFINE(m_sensor_work, sensor_work_handler);

#if defined(CONFIG_W1)
/* #485 S4: the boot scan was the only automatic rebind, so a probe plugged in
 * after boot (or one that missed the boot scan) stayed no_data until a reboot.
 *
 * While a taught slot is unbound, a rescan runs on a backoff of 2, 5, 10 and
 * 15 min, then every 15 min. Each one is a bare ROM search (a few ms, no device
 * init or read). The full scan + rebind (~1 s: probe re-init plus a throwaway
 * read per device) runs only when the set of ROMs on the bus differs from the
 * one the last rebind saw, and a fresh sample follows only when that rebind
 * bound more slots. A fully bound bus is never touched. Everything runs on the
 * sensor work queue, so a scan never reshuffles driver indices under a sample. */
static const uint16_t W1_RESCAN_BACKOFF_S[] = {120, 300, 600, 900};

struct w1_bus_sig {
	uint64_t sum; /* order-independent sum of the full ROM codes */
	uint16_t count;
};

static struct w1_bus_sig m_w1_sig; /* bus seen by the last rebind */
static int m_w1_present;           /* slots bound by the last rebind */
static uint8_t m_w1_rescan_step;
static bool m_w1_rescan_force;

static int w1_sig_callback(struct w1_rom rom, void *user_data)
{
	struct w1_bus_sig *sig = user_data;

	sig->sum += w1_rom_to_uint64(&rom);
	sig->count++;
	return 0;
}

/* Bare ROM search of the DS2484 bus, nothing else on the bus is touched. */
static int w1_bus_sig_read(struct w1_bus_sig *sig)
{
	static const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));
	static struct app_w1 w1;
	int ret;
	int res;

	*sig = (struct w1_bus_sig){0};

	ret = app_w1_acquire(&w1, dev);
	if (ret) {
		(void)app_w1_release(&w1, dev);
		return ret;
	}

	res = app_w1_scan(&w1, dev, w1_sig_callback, sig);

	ret = app_w1_release(&w1, dev);
	return res ? res : ret;
}

/* Scan every 1-Wire driver and rebind the slots by ROM. Returns the first scan
 * error; the rebind still runs on what was found. */
static int w1_scan_rebind(void)
{
	int ret;
	int res = 0;

	/* Remember the bus this rebind sees; a failed search forces the next
	 * rescan to do the full scan again. */
	if (w1_bus_sig_read(&m_w1_sig)) {
		m_w1_sig.count = UINT16_MAX;
	}

	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ds18b20_scan", ret);
		res = ret;
	}

#if defined(CONFIG_DS28E17)
	ret = app_machine_probe_scan();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_machine_probe_scan", ret);
		res = res ? res : ret;
	}
#endif /* defined(CONFIG_DS28E17) */

	m_w1_present = app_w1_slots_rebind();

	LOG_INF("1-Wire slots: %d sensor(s) bound", m_w1_present);
	return res;
}

static void w1_rescan_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(m_w1_rescan_work, w1_rescan_work_handler);

/* Arm the next backoff step while a taught slot is unbound, else end the
 * episode. */
static void w1_rescan_arm_next(void)
{
	if (!app_w1_slots_need_rescan()) {
		m_w1_rescan_step = 0;
		return;
	}

	uint16_t delay = W1_RESCAN_BACKOFF_S[m_w1_rescan_step];

	if (m_w1_rescan_step < ARRAY_SIZE(W1_RESCAN_BACKOFF_S) - 1) {
		m_w1_rescan_step++;
	}
	(void)k_work_schedule_for_queue(&m_sensor_work_q, &m_w1_rescan_work, K_SECONDS(delay));
}

static void w1_rescan_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	bool force = m_w1_rescan_force;

	m_w1_rescan_force = false;

	if (!g_app_config.cap_w1_sensors || !app_w1_slots_any_taught()) {
		m_w1_rescan_step = 0;
		return;
	}

	if (force || app_w1_slots_need_rescan()) {
		struct w1_bus_sig sig;
		bool changed = w1_bus_sig_read(&sig) != 0 || sig.count != m_w1_sig.count ||
			       sig.sum != m_w1_sig.sum;

		LOG_INF("1-Wire rescan: %u device(s) on the bus, %s", sig.count,
			force || changed ? "rebind" : "unchanged");

		if (force || changed) {
			int before = m_w1_present;

			(void)w1_scan_rebind();

			/* Fresh readings only for a newly bound sensor. */
			if (m_w1_present > before) {
				app_sensor_sample();
			}
		}
	}

	w1_rescan_arm_next();
}
#endif /* defined(CONFIG_W1) */

void app_sensor_w1_rescan(bool force)
{
#if defined(CONFIG_W1)
	if (force) {
		m_w1_rescan_force = true;
		m_w1_rescan_step = 0;
	}
	(void)k_work_reschedule_for_queue(&m_sensor_work_q, &m_w1_rescan_work, K_NO_WAIT);
#else
	ARG_UNUSED(force);
#endif
}

static void sensor_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_sensor_work_q, &m_sensor_work);
}

static K_TIMER_DEFINE(m_sensor_timer, sensor_timer_handler, NULL);

#if defined(CONFIG_APP_PYQ1648)
static void pyq1648_event_handler(void *user_data)
{
	LOG_INF("Motion detected");

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	APP_SENSOR_MB_U(&g_app_sensor_data, PIR_COUNT)++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION, true);
}
#endif /* defined(CONFIG_APP_PYQ1648) */

#if defined(CONFIG_LIS2DH)
/* Event-classification thresholds on total acceleration magnitude (m/s^2).
 * Free-fall ≈ weightless: in the air all axes drop toward 0 g, so |a| collapses
 * well below 1 g (9.81). An impact is a sharp shock far above 1 g. Anything in
 * between is ordinary motion. The any-motion (slope) interrupt fires on the
 * 1 g→0 g edge at the START of a fall and on an impact's shock; reading |a|
 * right after the trigger then tells the three cases apart. */
#define ACCEL_FREEFALL_MS2 4.0f  /* |a| below this ≈ free-fall (~0.4 g) */
#define ACCEL_IMPACT_MS2   25.0f /* |a| above this = impact/shock (~2.5 g) */

/* Classify the event by total acceleration. Runs on the system workqueue, NOT
 * in the trigger handler: app_accel_read() does a sensor_sample_fetch() on the
 * same LIS2DH, which must not be called re-entrantly from inside that sensor's
 * own trigger callback (it would deadlock the driver). A few ms of latency vs
 * the interrupt is acceptable for logging the kind. */
static struct k_work m_accel_classify_work;

static void accel_classify_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	float ax, ay, az;
	if (app_accel_read(&ax, &ay, &az, NULL) != 0) {
		return;
	}

	float mag = sqrtf(ax * ax + ay * ay + az * az);
	const char *kind = "motion";
	if (mag < ACCEL_FREEFALL_MS2) {
		kind = "free-fall";
	} else if (mag > ACCEL_IMPACT_MS2) {
		kind = "impact";
	}
	LOG_INF("Accel event class: %s (|a|=%d.%02d m/s^2)", kind, (int)mag,
		(int)(mag * 100) % 100);
}

/* LIS2DH any-motion interrupt (mirrors the PIR path): count the event, raise
 * the accelerometer-motion alarm source, and kick off async classification.
 * Must stay light — it runs in the driver's trigger context. The alarm layer
 * handles the orange LED, the rate-limited uplink (alarm_limit) and the
 * red-LED hold. */
static void accel_motion_handler(void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("Accelerometer motion detected");

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	APP_SENSOR_MB_U(&g_app_sensor_data, ACCEL_COUNT)++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_ACCEL_MOTION, true);
	k_work_submit(&m_accel_classify_work);
}
#endif /* defined(CONFIG_LIS2DH) */

void app_sensor_suspend(void)
{
	/* Stop the periodic sample timer so no further I2C/1-Wire/ADC activity is
	 * scheduled ahead of a deep-sleep poweroff. */
	k_timer_stop(&m_sensor_timer);
}

bool app_sensor_i2c_wedged(void)
{
	/* Non-zero streak = the most recent sweep saw every I2C read fail, i.e. the
	 * bus is currently wedged (cleared on the next good sweep or after recovery).
	 * #340 M19: m_i2c_fail_streak is now folded into g_app_sensor_data_lock's
	 * critical section (app_sensor_sample()), so read it under the same lock
	 * instead of relying on a plain int read being "close enough" - callers
	 * (app_cmd.c, a different thread) get a consistent snapshot rather than a
	 * torn/racy one. */
	bool wedged;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	wedged = m_i2c_fail_streak > 0;
	k_mutex_unlock(&g_app_sensor_data_lock);

	return wedged;
}

int app_sensor_init(void)
{
	int ret;
	int res = 0;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	mb_counters_init();
	k_mutex_unlock(&g_app_sensor_data_lock);

#if defined(CONFIG_OPT3001)
	if (g_app_config.cap_light_sensor) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "opt3001", ret);
			res = res ? res : ret;
		}
	}
#endif /* defined(CONFIG_OPT3001) */

	if (g_app_config.cap_barometer) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "mpl3115a2", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_init", ret);
			res = res ? res : ret;
		}
	}

	/* #498: PIR, the buzzer HW variant (#338) and the inputs share PB4/PA11
	 * (#90); cap_gpio_mode names the one owner, so no conflict is stored.
	 * Normalize the runtime copy only (g_app_config; the persisted config is
	 * untouched) to what this build and mode can actually drive, so telemetry,
	 * the channel registry and the settings-info report the effective use. */
	if (g_app_config.cap_gpio_mode != APP_CONFIG_GPIO_MODE_INPUTS) {
		g_app_config.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_DISABLED;
		g_app_config.cap_gpio_input_b = APP_CONFIG_GPIO_INPUT_DISABLED;
	}
#if !defined(CONFIG_ADC)
	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_ANALOG ||
	    g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_ANALOG) {
		LOG_WRN("Analog input set but ADC not built in (CONFIG_ADC=n)");
		if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_ANALOG) {
			g_app_config.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_DISABLED;
		}
		if (g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_ANALOG) {
			g_app_config.cap_gpio_input_b = APP_CONFIG_GPIO_INPUT_DISABLED;
		}
	}
#endif /* !defined(CONFIG_ADC) */
#if !defined(CONFIG_APP_PYQ1648)
	if (app_sensor_gpio_pir()) {
		/* #395: don't leave a configured PIR silently dead on a lean bench
		 * build — no motion events would look like a HW fault otherwise. */
		LOG_WRN("cap-gpio-mode pir but PIR not built in (CONFIG_APP_PYQ1648=n)");
		g_app_config.cap_gpio_mode = APP_CONFIG_GPIO_MODE_NONE;
	}
#endif /* !defined(CONFIG_APP_PYQ1648) */
#if !defined(CONFIG_APP_BUZZER)
	if (app_sensor_gpio_buzzer()) {
		LOG_WRN("cap-gpio-mode buzzer but buzzer not built in (CONFIG_APP_BUZZER=n)");
		g_app_config.cap_gpio_mode = APP_CONFIG_GPIO_MODE_NONE;
	}
#endif /* !defined(CONFIG_APP_BUZZER) */

	if (app_sensor_gpio_is_input(g_app_config.cap_gpio_input_a) ||
	    app_sensor_gpio_is_input(g_app_config.cap_gpio_input_b)) {
		ret = app_input_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_init", ret);
			res = res ? res : ret;
		}
	}

#if defined(CONFIG_ADC)
	/* After app_input_init(), which configures the pins it reads. */
	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_ANALOG ||
	    g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_ANALOG) {
		ret = app_analog_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_analog_init", ret);
			res = res ? res : ret;
		}
	}
#endif /* defined(CONFIG_ADC) */

#if defined(CONFIG_APP_PYQ1648)
	if (app_sensor_gpio_pir()) {
		ret = app_pyq1648_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_pyq1648_init", ret);
			res = res ? res : ret;
		} else {
			app_pyq1648_set_callback(pyq1648_event_handler, NULL);
		}
	}
#endif /* defined(CONFIG_APP_PYQ1648) */
#if defined(CONFIG_APP_BUZZER)
	if (app_sensor_gpio_buzzer()) {
		ret = app_buzzer_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_buzzer_init", ret);
			res = res ? res : ret;
		}
	}
#endif /* defined(CONFIG_APP_BUZZER) */

#if defined(CONFIG_LIS2DH)
	k_work_init(&m_accel_classify_work, accel_classify_work_handler);
	/* The accelerometer is a runtime capability: only arm motion + free-fall
	 * (and read orientation, see app_sensor_sample) when cap-accelerometer is on. */
	if (g_app_config.cap_accelerometer) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lis2dh12));

		/* lis2dh12 is deferred-init: power it up only when the cap is on,
		 * otherwise it would run ODR_5 low-power sampling permanently. */
		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "lis2dh12", ret);
			res = res ? res : ret;
		}

		/* Hand the part to runtime PM so it idles in power-down (ODR=0) and
		 * is only resumed on demand: for an orientation read (app_accel_read)
		 * or while interrupt detection is armed (app_accel_set_motion_
		 * sensitivity holds a get). device_init() above left it running at
		 * ODR_5 — enabling runtime PM here suspends it immediately (refcount
		 * 0), removing the ~continuous accelerometer current when detection is
		 * OFF. */
		ret = pm_device_runtime_enable(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("pm_device_runtime_enable", "lis2dh12", ret);
			res = res ? res : ret;
		}

		ret = app_accel_init_motion(accel_motion_handler, NULL);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_accel_init_motion", ret);
			res = res ? res : ret;
		}
	}
#endif /* defined(CONFIG_LIS2DH) */

#if defined(CONFIG_W1)
	if (g_app_config.cap_w1_sensors) {
		/* The DS2484 1-Wire master's device reset spans several back-to-back I2C
		 * transfers. i2c1 runtime PM would suspend the bus between them (gate the
		 * peripheral clock + apply the analog sleep pinctrl), which corrupts the
		 * multi-transfer sequence and makes the reset fail with -EIO (#224). Hold
		 * i2c1 resumed across the reset — but only across it.
		 *
		 * #329: this hold used to be permanent (never put), which pinned i2c1's
		 * runtime-PM usage count above zero for the whole uptime. That removed the
		 * only PM_DEVICE_ACTION_RESUME edge i2c1 ever gets, and that edge is what
		 * re-applies the bus configuration after a Stop2 — which on STM32WLE5
		 * wipes the I2C peripheral's registers (measured: CR1=0, TIMINGR=0 while
		 * RCC I2C1EN and the PB6/PB7 AF mux survive). With no resume, TIMINGR
		 * stayed 0 and *every* i2c1 slave NACKed for the rest of the boot: the
		 * ST25DV (NFC unreachable), the on-board SHT4x, and the DS2484 itself. The
		 * per-transaction hold in app_w1_acquire()/app_w1_release() keeps #224
		 * fixed while letting the bus suspend — and therefore self-heal — in
		 * between.
		 *
		 * The Stop lock goes with the hold for the same reason as in
		 * app_w1_acquire(): while i2c1 is pinned resumed there is no resume edge
		 * left to re-apply TIMINGR, so a Stop landing inside the DS2484 reset
		 * sequence would wipe the bus configuration with no way back. */
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		(void)pm_device_runtime_get(m_i2c_dev);

		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds2484", ret);
			res = res ? res : ret;
		}

		(void)pm_device_runtime_put(m_i2c_dev);
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	}

	/* Bring up the 1-Wire *sensor* drivers whenever cap-w1-sensors is on — even
	 * before any slot is taught — so on-demand discovery (`w1 scan` / the NFC
	 * W1Scan command) can register a freshly-plugged sensor. Previously these were
	 * gated on w1_taught, which made the FIRST sensor impossible to enroll: the
	 * scan callbacks (app_ds18b20_scan / app_machine_probe_scan -> scan_callback)
	 * need the sensor device ready, so on an untaught unit `w1 scan` found nothing
	 * and `w1 enroll` bound nothing (M7). The driver init is pure software (it only
	 * checks the DS2484 master is ready), so initialising devices for empty slots
	 * is harmless; the physical bus is only touched by the scans below. */
	if (g_app_config.cap_w1_sensors) {
		ret = device_init(DEVICE_DT_GET(DT_NODELABEL(ds18b20_0)));
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_0", ret);
			res = res ? res : ret;
		}

		ret = device_init(DEVICE_DT_GET(DT_NODELABEL(ds18b20_1)));
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_1", ret);
			res = res ? res : ret;
		}

#if defined(CONFIG_DS28E17)
		ret = device_init(DEVICE_DT_GET(DT_NODELABEL(machine_probe_0)));
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_0", ret);
			res = res ? res : ret;
		}

		ret = device_init(DEVICE_DT_GET(DT_NODELABEL(machine_probe_1)));
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_1", ret);
			res = res ? res : ret;
		}
#endif /* defined(CONFIG_DS28E17) */
	}
#endif /* defined(CONFIG_W1) */

	/* Boot-time scan + slot rebind runs only when a slot is already taught: an
	 * untaught unit has nowhere to bind results (the no-data watchdog keys on
	 * taught ROMs), so scanning at boot is just log spam. First enrollment now
	 * goes through on-demand `w1 scan` (the sensor devices above are ready). Tilt
	 * alert is armed per probe inside app_machine_probe_scan() (scan_callback), so
	 * it survives on-demand re-scans/enrolls too. */
#if defined(CONFIG_W1)
	if (g_app_config.cap_w1_sensors && app_w1_slots_any_taught()) {
		ret = w1_scan_rebind();
		res = res ? res : ret;
	}
#endif /* defined(CONFIG_W1) */

	k_work_queue_init(&m_sensor_work_q);

	k_work_queue_start(&m_sensor_work_q, m_sensor_work_stack,
			   K_THREAD_STACK_SIZEOF(m_sensor_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

#if defined(CONFIG_W1)
	/* #485 S4: start the rescan backoff if the boot rebind left a slot unbound. */
	if (g_app_config.cap_w1_sensors && app_w1_slots_any_taught()) {
		w1_rescan_arm_next();
	}
#endif /* defined(CONFIG_W1) */

	if (g_app_config.interval_sample) {
		k_timer_start(&m_sensor_timer, K_SECONDS(1),
			      K_SECONDS(g_app_config.interval_sample));
	}

	/* L-51: take one full sample so g_app_sensor_data holds real readings before
	 * telemetry is composed (otherwise the first report — which may fire on the
	 * join edge before the 1 s sensor-timer tick — would ship the pre-sample
	 * sentinels, e.g. voltage 0 → a bogus low-battery on the dashboard).
	 *
	 * Submit it to the sensor work queue instead of running it inline:
	 * app_sensor_sample() does a battery ADC read, and a synchronous ADC read
	 * this early in boot hangs the release build (the ADC clock has not settled
	 * yet; CONFIG_LOG timing hides it in debug), starving the watchdog and
	 * reset-looping before main(). Deferring lets init finish; the sample (with a
	 * valid battery reading) then runs once on the work queue, well after the ADC
	 * is ready, and populates the cache before DeviceInfo is sent. */
	k_work_submit_to_queue(&m_sensor_work_q, &m_sensor_work);

	return res;
}

void app_sensor_sample_async(void)
{
	(void)k_work_submit_to_queue(&m_sensor_work_q, &m_sensor_work);
}

void app_sensor_sample(void)
{
	int ret;

	int orientation = INT_MAX;
	float voltage = NAN;

	float temperature = NAN;
	float humidity = NAN;
	float illuminance = NAN;
	float altitude = NAN;
	float pressure = NAN;
	float baro_temperature = NAN;
	float input_a_voltage = NAN;
	float input_b_voltage = NAN;

	struct app_hall_data hall_data = {0};
	struct app_input_data input_data = {0};

	/* Track I2C sensor reads this sweep to detect a wedged bus. */
	int i2c_tried = 0, i2c_failed = 0;

	/* #340 M18: set false only when SHT4x rejects a CRC-valid, comm-successful
	 * reading as outside its plausible window (-ERANGE) - see below. Stays
	 * true (write-through, same as today) whenever CONFIG_SHT4X is off. */
	bool sht4x_valid = true;

	/* #485 S2: which onboard sensor's read failed this sweep (comm error). */
	bool failed[MB_SRC_COUNT] = {false};

	struct app_sensor_w1 w1[APP_W1_SLOT_COUNT];
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		app_sensor_w1_clear(&w1[s], APP_SENSOR_TYPE_NONE);
	}

#if defined(CONFIG_ADC)
	ret = app_battery_measure(&voltage);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_battery_measure", ret);
	}

	/* #396: app_sensor_init() resolved the pin use (#498), so an analog pin
	 * mode means the pin is in analog mode. A failed read leaves the channel NaN. */
	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_ANALOG) {
		ret = app_analog_measure_a(&input_a_voltage);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_analog_measure_a", ret);
		}
	}
	if (g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_ANALOG) {
		ret = app_analog_measure_b(&input_b_voltage);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_analog_measure_b", ret);
		}
	}
#endif /* defined(CONFIG_ADC) */

#if defined(CONFIG_LIS2DH)
	if (g_app_config.cap_accelerometer) {
		ret = app_accel_read(NULL, NULL, NULL, &orientation);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			failed[MB_SRC_ACCEL] = true;
			LOG_ERR_CALL_FAILED_INT("app_accel_read", ret);
		}
	}
	/* orientation stays INT_MAX when the capability is off → absent in telemetry */
#endif /* defined(CONFIG_LIS2DH) */

#if defined(CONFIG_SHT4X)
	/* #465: cap_sht off skips the read and keeps it out of the wedged-bus
	 * accounting; temperature/humidity stay NaN (written through below). */
	if (g_app_config.cap_sht) {
		ret = app_sht4x_read(&temperature, &humidity);
		if (ret == -ERANGE) {
			/* #340 M18: comm succeeded and the CRC was valid - this is a
			 * plausibility rejection, not a bus failure. Don't count it
			 * toward the wedged-bus streak below (a real, working bus that
			 * just returned one implausible-but-valid sample isn't
			 * "wedged"), and don't let the NaN temperature/humidity locals
			 * overwrite the last known-good reading further down - that
			 * would otherwise trip the no-data alarm (APP_ALARM_NO_DATA_MS)
			 * off one implausible sample, exactly like a genuinely
			 * disconnected sensor. */
			sht4x_valid = false;
			LOG_WRN("SHT4x reading rejected as implausible; keeping last value");
		} else {
			i2c_tried++;
			if (ret) {
				i2c_failed++;
				failed[MB_SRC_SHT] = true;
				LOG_ERR_CALL_FAILED_INT("app_sht4x_read", ret);
			}
		}
	}
#endif /* defined(CONFIG_SHT4X) */

#if defined(CONFIG_OPT3001)
	if (g_app_config.cap_light_sensor) {
		ret = app_opt3001_read(&illuminance);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			failed[MB_SRC_LIGHT] = true;
			LOG_ERR_CALL_FAILED_INT("app_opt3001_read", ret);
		}
	}
#endif /* defined(CONFIG_OPT3001) */

	if (g_app_config.cap_barometer) {
		ret = app_mpl3115a2_read(&altitude, &pressure, &baro_temperature);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			failed[MB_SRC_BARO] = true;
			LOG_ERR_CALL_FAILED_INT("app_mpl3115a2_read", ret);
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_get_data(&hall_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_get_data", ret);
		}
	}

	if (app_sensor_gpio_is_input(g_app_config.cap_gpio_input_a) ||
	    app_sensor_gpio_is_input(g_app_config.cap_gpio_input_b)) {
		ret = app_input_get_data(&input_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_get_data", ret);
		}
	}

	/* Read each logical 1-Wire slot through its ROM-bound driver (app_w1_slots
	 * dispatches on the slot type). Unbound / absent slots return present=false
	 * with NaN channels, so a slot keeps a stable identity across reboots
	 * regardless of bus enumeration order. */
#if defined(CONFIG_W1)
	if (g_app_config.cap_w1_sensors) {
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			ret = app_w1_slots_read(s, &w1[s]);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_w1_slots_read", ret);
				continue;
			}
			if (w1[s].present) {
				float t = app_sensor_w1_f(&w1[s],
							  APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE);
				float h = app_sensor_w1_f(&w1[s],
							  APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY);
				bool tilt =
					app_sensor_w1_f(&w1[s], APP_SENSOR_CH_MACHINE_PROBE_TILT) ==
					1.0f;

				LOG_INF("Slot %d / Temperature: %s%d.%02d C / Humidity: %s%d.%01d "
					"%% "
					"/ Tilt: %sactive",
					s, APP_FP2(t), APP_FP1(h), tilt ? "" : "not ");
			}
		}
	}
#endif /* defined(CONFIG_W1) */

	/* The motherboard channel vector for this sweep (#430). Physical units per
	 * app_sensor_types.yaml: the MPL3115A2 reports kPa, the channel is hPa. */
	struct app_sensor_mb mb = {.valid = 0};

	for (size_t ch = 0; ch < ARRAY_SIZE(mb.v); ch++) {
		mb.v[ch].f = NAN;
	}
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, temperature);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, humidity);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_PRESSURE, pressure * 10.0f);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_ILLUMINANCE, illuminance);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE_BARO, baro_temperature);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_ALTITUDE, altitude);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE, voltage);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_A_VOLTAGE, input_a_voltage);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_B_VOLTAGE, input_b_voltage);
	mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION,
	       orientation == INT_MAX ? NAN : (float)orientation);

	if (g_app_config.cap_hall_left) {
		mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE,
		       hall_data.left_is_active ? 1.0f : 0.0f);
	}
	if (g_app_config.cap_hall_right) {
		mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_HALL_RIGHT_STATE,
		       hall_data.right_is_active ? 1.0f : 0.0f);
	}
	/* An S0 pin is a pulse counter only: its level is not a state. */
	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_DIGITAL) {
		mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE,
		       input_data.input_a_is_active ? 1.0f : 0.0f);
	}
	if (g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_DIGITAL) {
		mb_put(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_B_STATE,
		       input_data.input_b_is_active ? 1.0f : 0.0f);
	}
	/* Counters are exact integers; they read 0 while their capability is off,
	 * as before the channel model. */
	mb_put_u(&mb, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT, hall_data.left_count);
	mb_put_u(&mb, APP_SENSOR_CH_MOTHERBOARD_HALL_RIGHT_COUNT, hall_data.right_count);
	mb_put_u(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_A_COUNT, input_data.input_a_count);
	mb_put_u(&mb, APP_SENSOR_CH_MOTHERBOARD_INPUT_B_COUNT, input_data.input_b_count);

	/* #340 M19: m_i2c_fail_streak is read/written by app_sensor_i2c_wedged()
	 * (app_cmd.c, a different thread context) with no protection of its own;
	 * folding it into the same g_app_sensor_data_lock critical section as the
	 * rest of this sweep's results (acquired below, instead of only around
	 * the data-copy that used to follow) closes that gap without adding a
	 * second lock. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	/* A whole sweep where every attempted I2C read failed points at a wedged
	 * bus (slave holding SDA low). After a few such sweeps, bit-bang it free —
	 * otherwise every I2C sensor stays dead until a reboot that may never come. */
	if (i2c_tried > 0 && i2c_failed == i2c_tried) {
		if (++m_i2c_fail_streak >= I2C_RECOVER_FAIL_THRESHOLD) {
			LOG_WRN("All %d I2C reads failed for %d sweeps; recovering bus", i2c_tried,
				m_i2c_fail_streak);
			if (device_is_ready(m_i2c_dev)) {
				ret = i2c_recover_bus(m_i2c_dev);
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("i2c_recover_bus", ret);
				}
			}
			m_i2c_fail_streak = 0;
		}
	} else {
		m_i2c_fail_streak = 0;
	}

	struct app_sensor_mb *g = &g_app_sensor_data.mb;

	/* Keep the last known-good reading instead of this sweep's NaN locals:
	 * - #340 M18: on an SHT4x plausibility reject (always);
	 * - #485 S2: on a failed read, for APP_SENSOR_MISS_HOLD sweeps. A single
	 *   transient I2C error would otherwise leave NaN for a whole sample
	 *   interval (900 s by default), long past the no-data window, and raise
	 *   no_data plus a DEACTIVATE/ACTIVATE pair on every threshold rule of the
	 *   channel. A second failure in a row goes NaN, as before. */
	static const uint32_t src_mask[MB_SRC_COUNT] = {
		[MB_SRC_ACCEL] = BIT(APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION),
		[MB_SRC_SHT] = BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE) |
			       BIT(APP_SENSOR_CH_MOTHERBOARD_HUMIDITY),
		[MB_SRC_LIGHT] = BIT(APP_SENSOR_CH_MOTHERBOARD_ILLUMINANCE),
		[MB_SRC_BARO] = BIT(APP_SENSOR_CH_MOTHERBOARD_PRESSURE) |
				BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE_BARO) |
				BIT(APP_SENSOR_CH_MOTHERBOARD_ALTITUDE),
	};
	uint32_t keep = sht4x_valid ? 0 : src_mask[MB_SRC_SHT];

	for (int i = 0; i < MB_SRC_COUNT; i++) {
		keep |= app_sensor_miss_hold(&m_mb_miss[i], failed[i], src_mask[i]);
	}
	app_sensor_mb_keep(&mb, g, keep);

	/* The PIR / accel event counters are owned by their event handlers, not by
	 * this sweep — carry them over. */
	mb.v[APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT] = g->v[APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT];
	mb.v[APP_SENSOR_CH_MOTHERBOARD_ACCEL_COUNT] = g->v[APP_SENSOR_CH_MOTHERBOARD_ACCEL_COUNT];
	mb.valid |= BIT(APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT) |
		    BIT(APP_SENSOR_CH_MOTHERBOARD_ACCEL_COUNT);

	*g = mb;
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		g_app_sensor_data.w1[s] = w1[s];
	}

	k_mutex_unlock(&g_app_sensor_data_lock);
}
