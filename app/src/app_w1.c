/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_w1.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/w1.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_w1, LOG_LEVEL_DBG);

#define ACQUIRE_DELAY K_MSEC(3)

/* The DS2484 sits on i2c1 and every 1-Wire operation is several back-to-back I2C
 * transfers with short sleeps between them. i2c1 runtime PM would suspend the bus
 * in those gaps (gate the peripheral clock + park SCL/SDA in analog mode), which
 * corrupts the sequence and fails with -EIO (#224). So the bus is held resumed for
 * the whole acquire..release window — and released again afterwards, which is the
 * part that matters for #329: the release lets i2c1 suspend, so the next transfer
 * produces a PM_DEVICE_ACTION_RESUME edge, and that edge is what re-applies the
 * bus configuration (TIMINGR) after a Stop2 has wiped the I2C registers. Holding
 * it permanently, as app_sensor.c used to, removed that edge and left every i2c1
 * slave NACKing for the rest of the boot. */
static const struct device *const m_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

/* Guards the i2c1 runtime-PM reference so it is taken and released exactly once
 * per acquire..release window. Needed because callers invoke app_w1_release()
 * even when app_w1_acquire() failed (the same reason w1_unlock_bus then returns
 * -EINVAL), so a naive put in both paths double-releases and trips
 * "pm_device: Unbalanced suspend", leaving the bus suspended under a caller that
 * still thinks it owns it. Same one-shot idiom as m_awake_held in app_nfc.c. */
static atomic_t m_i2c_held;

static void i2c_hold_get(void)
{
	if (atomic_cas(&m_i2c_held, 0, 1)) {
		/* Block every Stop state for exactly as long as i2c1 is pinned resumed.
		 * These two have to go together: a Stop2 wipes the I2C1 registers, and the
		 * only thing that re-applies them is the PM_DEVICE_ACTION_RESUME edge that
		 * this very hold suppresses. Without the policy lock, a Stop2 landing in
		 * the ACQUIRE_DELAY sleep below (3 ms, well past the 900 us stop2
		 * min-residency) leaves TIMINGR at 0 with no way back, and every transfer
		 * for the rest of the window NACKs -- which is why the DS2484 failed while
		 * the SHT4x, whose transfer gets its own resume edge, did not.
		 * i2c_stm32_transfer() takes this identical lock per transfer; this just
		 * extends the cover to the gaps between them. Refcounted, so it nests. */
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		(void)pm_device_runtime_get(m_i2c_dev);
	}
}

static void i2c_hold_put(void)
{
	if (atomic_cas(&m_i2c_held, 1, 0)) {
		/* Strict LIFO: let the bus suspend first (that is what arms the resume edge
		 * for the next transfer), then allow Stop again. */
		(void)pm_device_runtime_put(m_i2c_dev);
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	}
}

/* Upper bound on devices recorded in a single bus scan (#201). The real bus
 * holds a handful of sensors; this caps per-ROM k_malloc so a noisy bus (EMI
 * phantom ROMs) can't exhaust the heap before the free loop in app_w1_scan
 * runs. Generously above any real population. */
#define APP_W1_SCAN_MAX 32

struct scan_item {
	struct w1_rom rom;
	sys_snode_t node;
};

int app_w1_acquire(struct app_w1 *w1, const struct device *dev)
{
	int ret;
	int res = 0;

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = w1_lock_bus(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("w1_lock_bus", ret);
		return ret;
	}

	/* Keep i2c1 resumed for the whole 1-Wire window (see m_i2c_dev above). */
	i2c_hold_get();

	ret = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		res = ret;
		goto error;
	}

	/*
	 * TODO This should be part of the bus driver. The driver
	 * must guarantee nobody will access the I2C bus during the delay.
	 */
	k_sleep(ACQUIRE_DELAY);

	ret = w1_reset_bus(dev);
	if (ret < 0) {
		LOG_ERR_CALL_FAILED_INT("w1_reset_bus", ret);
		res = ret;
		goto error;
	}

	return 0;

error:
	i2c_hold_put();

	ret = w1_unlock_bus(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("w1_unlock_bus", ret);
		res = res ? res : ret;
	}

	return res;
}

int app_w1_release(struct app_w1 *w1, const struct device *dev)
{
	int ret;
	int res = 0;

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* #340 L5: a failed app_w1_acquire() never actually took the bus lock / PM
	 * hold (its own error path already put m_i2c_held back to 0), so it may be
	 * held by a concurrent legitimate owner right now. Driving DS28E17 bus
	 * traffic or a PM_DEVICE_ACTION_SUSPEND below regardless would race that
	 * owner. m_i2c_held is exactly the acquire/release one-shot state this
	 * needs -- skip straight to the already-idempotent unlock when it's not
	 * held. */
	if (!atomic_get(&m_i2c_held)) {
		(void)w1_unlock_bus(dev);
		return 0;
	}

	if (w1->is_ds28e17_present) {
		ret = w1_reset_bus(dev);
		if (ret == 1) {
			const struct w1_slave_config config = {.overdrive = 0};
			ret = w1_skip_rom(dev, &config);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("w1_skip_rom", ret);
			} else {
				const uint8_t buf[] = {0x1e};
				ret = w1_write_block(dev, buf, sizeof(buf));
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("w1_write_block", ret);
				}
			}
		}
	}

	ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR_CALL_FAILED_INT("pm_device_action_run", ret);
		res = ret;
		goto error;
	}

error:
	/* Release i2c1 so it can suspend again — this is what restores the
	 * PM_DEVICE_ACTION_RESUME edge that re-applies TIMINGR after a Stop2 (#329).
	 * Placed after the label so both the success and error paths balance the get
	 * taken in app_w1_acquire(); the one-shot guard makes a release-after-failed-
	 * acquire a no-op rather than a double put. */
	i2c_hold_put();

	ret = w1_unlock_bus(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("w1_unlock_bus", ret);
		res = res ? res : ret;
	}

	return res;
}

static void w1_search_callback(struct w1_rom rom, void *cb_arg)
{
	struct app_w1 *w1 = cb_arg;

	if (rom.family == 0x19) {
		w1->is_ds28e17_present = true;
	}

	if (w1->scan_count >= APP_W1_SCAN_MAX) {
		/* Drop further ROMs rather than keep allocating. Logged once at the
		 * cap so a flood of EMI phantoms can't spam the log either. */
		if (w1->scan_count == APP_W1_SCAN_MAX) {
			LOG_WRN("1-Wire scan cap %d reached; ignoring further devices",
				APP_W1_SCAN_MAX);
			w1->scan_count++;
		}
		return;
	}

	struct scan_item *item = k_malloc(sizeof(*item));
	if (!item) {
		LOG_ERR_CALL_FAILED("k_malloc");
		return;
	}

	item->rom = rom;
	w1->scan_count++;

	sys_slist_append(&w1->scan_list, &item->node);
}

int app_w1_scan(struct app_w1 *w1, const struct device *dev,
		int (*user_cb)(struct w1_rom rom, void *user_data), void *user_data)
{
	int ret;
	int res = 0;

	w1->is_ds28e17_present = false;
	w1->scan_count = 0;

	sys_slist_init(&w1->scan_list);

	ret = w1_search_rom(dev, w1_search_callback, w1);
	if (ret < 0) {
		LOG_ERR_CALL_FAILED_INT("w1_search_rom", ret);
		res = ret;
		goto error;
	}

	LOG_DBG("Found %d device(s)", ret);

	struct scan_item *item;
	struct scan_item *item_safe;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&w1->scan_list, item, item_safe, node) {
		if (user_cb) {
			ret = user_cb(item->rom, user_data);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("user_cb", ret);
				res = res ? res : ret;
			}
		}
	}

error:
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&w1->scan_list, item, item_safe, node) {
		sys_slist_remove(&w1->scan_list, NULL, &item->node);
		k_free(item);
	}

	return res;
}
