/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_alarm.h"
#include "app_battery.h"
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
#include "app_sht4x.h"
#include "app_w1_slots.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
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

struct app_sensor_data g_app_sensor_data = {
	.orientation = INT_MAX,
	.voltage = NAN,
	.temperature = NAN,
	.humidity = NAN,
	.illuminance = NAN,
	.altitude = NAN,
	.pressure = NAN,
	.w1 =
		{
			[0 ... APP_W1_SLOT_COUNT - 1] = {.temperature = NAN, .humidity = NAN},
		},
};

K_MUTEX_DEFINE(g_app_sensor_data_lock);

static K_THREAD_STACK_DEFINE(m_sensor_work_stack, 2048);
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

static void sensor_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_sensor_work_q, &m_sensor_work);
}

static K_TIMER_DEFINE(m_sensor_timer, sensor_timer_handler, NULL);

static void pyq1648_event_handler(void *user_data)
{
	LOG_INF("Motion detected");

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.motion_count++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	app_alarm_event(APP_ALARM_SRC_PIR, true);
}

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
	g_app_sensor_data.accel_motion_count++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	app_alarm_event(APP_ALARM_SRC_ACCEL, true);
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
	 * int read is atomic; a stale value only mislabels one Info snapshot. */
	return m_i2c_fail_streak > 0;
}

int app_sensor_init(void)
{
	int ret;
	int res = 0;

	if (g_app_config.cap_light_sensor) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "opt3001", ret);
			res = res ? res : ret;
		}
	}

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

	if ((g_app_config.cap_input_a || g_app_config.cap_input_b) &&
	    g_app_config.cap_pir_detector) {
		/* PIR and the digital inputs share PB4/PA11 (#90). PIR wins; the inputs
		 * are not initialised. Clear their runtime enables too (H-7) so telemetry
		 * and GetConfig stop reporting a non-functional input as active — otherwise
		 * the operator sees the input "enabled" with count 0 and never learns it is
		 * silently dead. The persisted config is untouched (g_app_config is the
		 * runtime copy); fixing the pin conflict is a config change. */
		LOG_WRN("PIR and input share GPIO pins — inputs disabled (PIR takes the pins)");
		g_app_config.cap_input_a = false;
		g_app_config.cap_input_b = false;
	} else if (g_app_config.cap_input_a || g_app_config.cap_input_b) {
		ret = app_input_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_init", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_pir_detector) {
		ret = app_pyq1648_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_pyq1648_init", ret);
			res = res ? res : ret;
		} else {
			app_pyq1648_set_callback(pyq1648_event_handler, NULL);
		}
	}

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

	/* Bring up the 1-Wire *sensor* drivers and scan the bus only when at least one
	 * slot is actually taught (a non-zero sensorN-rom). With cap-w1-sensors on but
	 * nothing enrolled the scan results have nowhere to go (no slot to bind, the
	 * no-data watchdog keys on taught ROMs), so it is wasted boot work and log
	 * spam. The DS2484 master above is still initialised whenever cap-w1-sensors is
	 * on, so on-demand discovery (`w1 scan` / the NFC W1Scan command, which scan
	 * the raw bus via the master) still works to teach a new sensor. */
	bool w1_taught = g_app_config.cap_w1_sensors && app_w1_slots_any_taught();

	if (w1_taught) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_0", ret);
			res = res ? res : ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_1", ret);
			res = res ? res : ret;
		}

		ret = app_ds18b20_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_ds18b20_scan", ret);
			res = res ? res : ret;
		}
	}

	if (w1_taught) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_0", ret);
			res = res ? res : ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_1", ret);
			res = res ? res : ret;
		}

		ret = app_machine_probe_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_machine_probe_scan", ret);
			res = res ? res : ret;
		}

		/* Tilt alert is armed per probe inside app_machine_probe_scan() (see
		 * scan_callback), so it survives runtime re-scans too — no separate
		 * arming pass is needed here. */
	}

	/* Bind discovered 1-Wire devices to logical slots by their persisted ROM
	 * (sensorN_rom), so a slot keeps the same physical sensor across reboots /
	 * rescans. Must run after both driver scans. */
	if (w1_taught) {
		int present = app_w1_slots_rebind();

		LOG_INF("1-Wire slots: %d sensor(s) bound", present);
	}

	k_work_queue_init(&m_sensor_work_q);

	k_work_queue_start(&m_sensor_work_q, m_sensor_work_stack,
			   K_THREAD_STACK_SIZEOF(m_sensor_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

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

	struct app_hall_data hall_data = {0};
	struct app_input_data input_data = {0};

	/* Track I2C sensor reads this sweep to detect a wedged bus. */
	int i2c_tried = 0, i2c_failed = 0;

	struct app_w1_slot_reading w1[APP_W1_SLOT_COUNT];
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		w1[s] = (struct app_w1_slot_reading){.temperature = NAN,
						     .humidity = NAN,
						     .is_tilt_alert = false,
						     .present = false};
	}

#if defined(CONFIG_ADC)
	ret = app_battery_measure(&voltage);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_battery_measure", ret);
	}
#endif /* defined(CONFIG_ADC) */

#if defined(CONFIG_LIS2DH)
	if (g_app_config.cap_accelerometer) {
		ret = app_accel_read(NULL, NULL, NULL, &orientation);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			LOG_ERR_CALL_FAILED_INT("app_accel_read", ret);
		}
	}
	/* orientation stays INT_MAX when the capability is off → absent in telemetry */
#endif /* defined(CONFIG_LIS2DH) */

#if defined(CONFIG_SHT4X)
	ret = app_sht4x_read(&temperature, &humidity);
	i2c_tried++;
	if (ret) {
		i2c_failed++;
		LOG_ERR_CALL_FAILED_INT("app_sht4x_read", ret);
	}
#endif /* defined(CONFIG_SHT4X) */

	if (g_app_config.cap_light_sensor) {
		ret = app_opt3001_read(&illuminance);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			LOG_ERR_CALL_FAILED_INT("app_opt3001_read", ret);
		}
	}

	if (g_app_config.cap_barometer) {
		ret = app_mpl3115a2_read(&altitude, &pressure, NULL);
		i2c_tried++;
		if (ret) {
			i2c_failed++;
			LOG_ERR_CALL_FAILED_INT("app_mpl3115a2_read", ret);
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_get_data(&hall_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_get_data", ret);
		}
	}

	if (g_app_config.cap_input_a || g_app_config.cap_input_b) {
		ret = app_input_get_data(&input_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_get_data", ret);
		}
	}

	/* Read each logical 1-Wire slot through its ROM-bound driver (app_w1_slots
	 * dispatches on the slot type). Unbound / absent slots return present=false
	 * with NaN readings, so a slot keeps a stable identity across reboots
	 * regardless of bus enumeration order. */
	if (g_app_config.cap_w1_sensors) {
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			ret = app_w1_slots_read(s, &w1[s]);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_w1_slots_read", ret);
				continue;
			}
			if (w1[s].present) {
				LOG_INF("Slot %d / Temperature: %s%d.%02d C / Humidity: %s%d.%01d "
					"%% "
					"/ Tilt: %sactive",
					s, APP_FP2(w1[s].temperature), APP_FP1(w1[s].humidity),
					w1[s].is_tilt_alert ? "" : "not ");
			}
		}
	}

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

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	g_app_sensor_data.orientation = orientation;
	g_app_sensor_data.voltage = voltage;

	g_app_sensor_data.temperature = temperature;
	g_app_sensor_data.humidity = humidity;
	g_app_sensor_data.illuminance = illuminance;
	g_app_sensor_data.altitude = altitude;
	g_app_sensor_data.pressure = pressure;

	g_app_sensor_data.hall_left_count = hall_data.left_count;
	g_app_sensor_data.hall_right_count = hall_data.right_count;
	g_app_sensor_data.hall_left_is_active = hall_data.left_is_active;
	g_app_sensor_data.hall_right_is_active = hall_data.right_is_active;

	g_app_sensor_data.input_a_count = input_data.input_a_count;
	g_app_sensor_data.input_b_count = input_data.input_b_count;
	g_app_sensor_data.input_a_is_active = input_data.input_a_is_active;
	g_app_sensor_data.input_b_is_active = input_data.input_b_is_active;

	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		g_app_sensor_data.w1[s] = w1[s];
	}

	k_mutex_unlock(&g_app_sensor_data_lock);
}
