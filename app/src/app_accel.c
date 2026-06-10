/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stddef.h>

LOG_MODULE_REGISTER(app_accel, LOG_LEVEL_DBG);

#define GRAVITY         9.80665f
#define ORIENTATION_THR 0.4f

static const int m_vectors[7][3] = {
	[0] = {0, 0, 0},  [1] = {-1, 0, 0}, [2] = {0, 0, 1}, [3] = {0, 1, 0},
	[4] = {0, -1, 0}, [5] = {0, 0, -1}, [6] = {1, 0, 0},
};

static int m_orientation;

static K_MUTEX_DEFINE(m_lock);

static void update_orientation(float coeff_x, float coeff_y, float coeff_z)
{
	int vector_x = m_vectors[m_orientation][0];
	int vector_y = m_vectors[m_orientation][1];
	int vector_z = m_vectors[m_orientation][2];

	if ((vector_x == 0 && (coeff_x < -ORIENTATION_THR || coeff_x > ORIENTATION_THR)) ||
	    (vector_x == 1 && (coeff_x < 1.f - ORIENTATION_THR)) ||
	    (vector_x == -1 && (coeff_x > -1.f + ORIENTATION_THR))) {
		goto update;
	}

	if ((vector_y == 0 && (coeff_y < -ORIENTATION_THR || coeff_y > ORIENTATION_THR)) ||
	    (vector_y == 1 && (coeff_y < 1.f - ORIENTATION_THR)) ||
	    (vector_y == -1 && (coeff_y > -1.f + ORIENTATION_THR))) {
		goto update;
	}

	if ((vector_z == 0 && (coeff_z < -ORIENTATION_THR || coeff_z > ORIENTATION_THR)) ||
	    (vector_z == 1 && (coeff_z < 1.f - ORIENTATION_THR)) ||
	    (vector_z == -1 && (coeff_z > -1.f + ORIENTATION_THR))) {
		goto update;
	}

	return;

update:

	for (int i = 1; i <= 6; i++) {
		float delta_x = fabsf(m_vectors[i][0] - coeff_x);
		float delta_y = fabsf(m_vectors[i][1] - coeff_y);
		float delta_z = fabsf(m_vectors[i][2] - coeff_z);

		if (delta_x < 1.f - ORIENTATION_THR && delta_y < 1.f - ORIENTATION_THR &&
		    delta_z < 1.f - ORIENTATION_THR) {
			m_orientation = i;
			return;
		}
	}
}

int app_accel_read(float *accel_x, float *accel_y, float *accel_z, int *orientation)
{
	int ret;

	k_mutex_lock(&m_lock, K_FOREVER);

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lis2dh12));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		k_mutex_unlock(&m_lock);
		return -ENODEV;
	}

	/* TODO Check if this is the best aprroach */
	for (int i = 0; i < 5; i++) {
		ret = sensor_sample_fetch(dev);

		if (ret != -ENODATA) {
			break;
		}

		k_sleep(K_MSEC(10));
	}

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_sample_fetch", ret);
		k_mutex_unlock(&m_lock);
		return ret;
	}

	struct sensor_value val[3];
	ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		k_mutex_unlock(&m_lock);
		return ret;
	}

	float accel_x_ = sensor_value_to_float(&val[0]);
	float accel_y_ = sensor_value_to_float(&val[1]);
	float accel_z_ = sensor_value_to_float(&val[2]);

	LOG_DBG("Acceleration X: %.3f m/s^2", (double)accel_x_);
	LOG_DBG("Acceleration Y: %.3f m/s^2", (double)accel_y_);
	LOG_DBG("Acceleration Z: %.3f m/s^2", (double)accel_z_);

	if (accel_x != NULL) {
		*accel_x = accel_x_;
	}

	if (accel_y != NULL) {
		*accel_y = accel_y_;
	}

	if (accel_z != NULL) {
		*accel_z = accel_z_;
	}

	update_orientation(accel_x_ / GRAVITY, accel_y_ / GRAVITY, accel_z_ / GRAVITY);

	int orientation_ = m_orientation;

	LOG_DBG("Orientation: %d", orientation_);

	if (orientation != NULL) {
		*orientation = orientation_;
	}

	k_mutex_unlock(&m_lock);

	return 0;
}

/* ---- Motion (any-motion) detection -------------------------------------- */

/* LIS2DH registers accessed directly — the sensor API has no equivalent for
 * the HP-filter setup nor for the REFERENCE dummy-read that re-bases it.
 * Datasheet (LIS2DH12): CTRL_REG2 (8.10), REFERENCE (8.15). */
#define LIS2DH_REG_CTRL2     0x21
#define LIS2DH_REG_REFERENCE 0x26
/* CTRL_REG2 = 0x02: HPM=00 (normal mode, reset by reading REFERENCE),
 * HPCF=00 (f_cut ~ ODR/50 = 0.2 Hz @ 10 Hz LP), FDS=0 (output registers stay
 * unfiltered — orientation in app_accel_read() keeps gravity), HP_IA2=1 (the
 * HP filter feeds the INT2/IA2 any-motion comparator). Without the HPF the
 * comparator sees the absolute acceleration including the static 1 g, which
 * permanently exceeds any sub-1g threshold. */
#define LIS2DH_CTRL2_HPF_IA2 0x02

/* Re-arm delay after every motion event (see motion_trigger_handler). Kept
 * short so a fall/impact arriving right after a prior event isn't dropped in
 * the disarm window; still bounds the interrupt rate against a storm. */
#define MOTION_REARM_MS         20
/* First arm after boot — by then the main loop feeds the watchdog and the
 * shell is up, so a mis-configured interrupt can degrade the device but
 * never brick the boot path. */
#define MOTION_BOOT_ARM_SECONDS 3

static const struct i2c_dt_spec m_lis2dh_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(lis2dh12));

static app_accel_motion_cb_t m_motion_cb;
static void *m_motion_user_data;

static struct sensor_trigger m_motion_trig = {
	.type = SENSOR_TRIG_DELTA,
	.chan = SENSOR_CHAN_ACCEL_XYZ,
};

static struct k_work_delayable m_motion_arm_work;

static void motion_arm_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int ret = app_accel_set_motion_sensitivity(g_app_config.accel_motion_sensitivity);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_accel_set_motion_sensitivity", ret);
	}
}

/* Enable the HP filter on the IA2 path and snap its reference to the current
 * acceleration: in HPM=00 mode a REFERENCE read re-bases the filter, so the
 * any-motion comparator sees ~0 at the instant the trigger is armed — no
 * settle time, gravity and orientation are nulled out (a static device must
 * never trigger). Serialized against app_accel_read() bus traffic by m_lock. */
static int motion_snap_hpf_reference(void)
{
	uint8_t ref;
	int ret;

	k_mutex_lock(&m_lock, K_FOREVER);
	ret = i2c_reg_write_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_CTRL2, LIS2DH_CTRL2_HPF_IA2);
	if (!ret) {
		ret = i2c_reg_read_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_REFERENCE, &ref);
	}
	k_mutex_unlock(&m_lock);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("motion_snap_hpf_reference", ret);
	}
	return ret;
}

/* Slope threshold (m/s^2 of the HP-filtered, i.e. dynamic, acceleration) and
 * duration (samples at the 100 Hz low-power ODR) per level. Retuned on HW: the
 * old 15/8/3 m/s^2 was too coarse — only "high" ever fired, medium/low never
 * triggered even on a desk-tap. New scale 7/4/2 m/s^2 is reachable by ordinary
 * motion and still rejects the static 1 g (HPF reference snap nulls gravity).
 * At 4 g FS the 7-bit INT2_THS LSB is ~0.307 m/s^2. */
static void sensitivity_params(enum app_config_motion_sensitivity level, int *th_ms2, int *dur)
{
	switch (level) {
	case APP_CONFIG_MOTION_SENSITIVITY_LOW:
		*th_ms2 = 7;
		*dur = 2;
		break;
	case APP_CONFIG_MOTION_SENSITIVITY_MEDIUM:
		*th_ms2 = 4;
		*dur = 1;
		break;
	case APP_CONFIG_MOTION_SENSITIVITY_HIGH:
		*th_ms2 = 2;
		*dur = 1;
		break;
	default:
		*th_ms2 = 0;
		*dur = 0;
		break;
	}
}

static void motion_trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(trig);

	/* Disarm at the driver level before anything else. The handler runs
	 * inside the driver's work item; returning with a handler still
	 * registered re-enables the GPIO interrupt (lis2dh_trigger.c), and a
	 * persistently asserted INT2 line then re-fires in a tight ISR/work
	 * loop on the system workqueue, starving the main loop and the
	 * watchdog feed. Disarm + delayed re-arm bounds the interrupt rate to
	 * 1/MOTION_REARM_SECONDS by construction, whatever the chip state. */
	int ret = sensor_trigger_set(dev, &m_motion_trig, NULL);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_trigger_set(disarm)", ret);
	}

	if (m_motion_cb) {
		m_motion_cb(m_motion_user_data);
	}

	k_work_schedule(&m_motion_arm_work, K_MSEC(MOTION_REARM_MS));
}

int app_accel_set_motion_sensitivity(enum app_config_motion_sensitivity level)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lis2dh12));
	int ret;

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	if (level == APP_CONFIG_MOTION_SENSITIVITY_OFF) {
		ret = sensor_trigger_set(dev, &m_motion_trig, NULL);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sensor_trigger_set(disable)", ret);
			return ret;
		}
		LOG_INF("Motion detection disabled");
		return 0;
	}

	int th_ms2, dur;
	sensitivity_params(level, &th_ms2, &dur);

	/* HPF reference snap must immediately precede arming so the comparator
	 * baseline matches the device's current attitude (also on every re-arm
	 * after a motion event and on runtime sensitivity changes). */
	ret = motion_snap_hpf_reference();
	if (ret) {
		return ret;
	}

	struct sensor_value th = {.val1 = th_ms2, .val2 = 0};
	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_TH, &th);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_attr_set(SLOPE_TH)", ret);
		return ret;
	}

	struct sensor_value sv_dur = {.val1 = dur, .val2 = 0};
	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_DUR, &sv_dur);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_attr_set(SLOPE_DUR)", ret);
		return ret;
	}

	ret = sensor_trigger_set(dev, &m_motion_trig, motion_trigger_handler);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_trigger_set", ret);
		return ret;
	}

	LOG_INF("Motion detection armed (level %d, th=%d m/s^2, dur=%d samples)", (int)level,
		th_ms2, dur);
	return 0;
}

/* ---- Free-fall detection (LIS2DH INT1 generator) ------------------------ */

/* The Zephyr driver only touches INT1 (gpio_drdy) for the data-ready trigger,
 * which we never register — so the INT1 generator and the INT1 pin are free
 * for free-fall, independent of the any-motion path on INT2. Datasheet 8.x. */
#define LIS2DH_REG_CTRL3         0x22
#define LIS2DH_REG_INT1_CFG      0x30
#define LIS2DH_REG_INT1_SRC      0x31
#define LIS2DH_REG_INT1_THS      0x32
#define LIS2DH_REG_INT1_DUR      0x33
#define LIS2DH_CTRL3_I1_IA1      0x40 /* route INT1 generator to the INT1 pin */
/* INT1_CFG = AOI(AND) + Z/Y/X low events: all three axes below threshold at
 * once = weightless = free-fall (the classic ST free-fall recipe). */
#define LIS2DH_INT1_CFG_FREEFALL 0x95
/* Threshold ~350 mg, min duration ~30 ms. At 4 g FS the INT1_THS LSB is
 * ~32 mg (the 16 mg figure is the 2 g value; cf. the any-motion LSB note above
 * = 0.307 m/s^2 = 31 mg, and the Zephyr lis2dh driver's 32 LSB/g at 4 g). So
 * 0x0B = 11 * 32 mg = 352 mg. The old 0x16 (22) was 704 mg, which INT1_CFG's
 * AND-of-all-axes-low could satisfy statically (~577 mg/axis on a diagonal),
 * firing a false free-fall on mere reorientation. The INT1_DURATION LSB is
 * 1/ODR (10 ms @ 100 Hz). Tune on HW. */
#define LIS2DH_FREEFALL_THS      0x0B
#define LIS2DH_FREEFALL_DUR      0x03

static const struct gpio_dt_spec m_int1_gpio =
	GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(lis2dh12), irq_gpios, 0);
static struct gpio_callback m_freefall_cb;
static struct k_work m_freefall_work;

static void freefall_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Read INT1_SRC to acknowledge the event flags. INT1 is not latched
	 * (CTRL_REG5 LIR_INT1 is never set), so the pin is level-following and the
	 * GPIO fires on the rising edge; this read just clears the IA/axis flags. */
	uint8_t src;
	k_mutex_lock(&m_lock, K_FOREVER);
	(void)i2c_reg_read_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_INT1_SRC, &src);
	k_mutex_unlock(&m_lock);

	LOG_INF("Free-fall detected");
	if (m_motion_cb) {
		m_motion_cb(m_motion_user_data);
	}
}

static void freefall_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&m_freefall_work);
}

/* Configure the INT1 generator for free-fall, route it to the INT1 pin and
 * hook our GPIO callback. Returns 0 also when there is no INT1 GPIO. */
static int app_accel_init_freefall(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_int1_gpio)) {
		LOG_ERR("INT1 GPIO not ready");
		return -ENODEV;
	}

	k_work_init(&m_freefall_work, freefall_work_handler);

	k_mutex_lock(&m_lock, K_FOREVER);
	ret = i2c_reg_write_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_INT1_THS, LIS2DH_FREEFALL_THS);
	if (!ret) {
		ret = i2c_reg_write_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_INT1_DUR,
					    LIS2DH_FREEFALL_DUR);
	}
	if (!ret) {
		/* Read-modify-write CTRL3: set I1_IA1, keep the driver's bits. */
		uint8_t ctrl3;
		ret = i2c_reg_read_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_CTRL3, &ctrl3);
		if (!ret) {
			ret = i2c_reg_write_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_CTRL3,
						    ctrl3 | LIS2DH_CTRL3_I1_IA1);
		}
	}
	if (!ret) {
		ret = i2c_reg_write_byte_dt(&m_lis2dh_i2c, LIS2DH_REG_INT1_CFG,
					    LIS2DH_INT1_CFG_FREEFALL);
	}
	k_mutex_unlock(&m_lock);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("free-fall register config", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_int1_gpio, GPIO_INPUT);
	if (!ret) {
		gpio_init_callback(&m_freefall_cb, freefall_isr, BIT(m_int1_gpio.pin));
		ret = gpio_add_callback_dt(&m_int1_gpio, &m_freefall_cb);
	}
	if (!ret) {
		ret = gpio_pin_interrupt_configure_dt(&m_int1_gpio, GPIO_INT_EDGE_RISING);
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("free-fall GPIO setup", ret);
		return ret;
	}

	LOG_INF("Free-fall detection enabled (INT1, ths=0x%02x, dur=0x%02x)", LIS2DH_FREEFALL_THS,
		LIS2DH_FREEFALL_DUR);
	return 0;
}

int app_accel_init_motion(app_accel_motion_cb_t cb, void *user_data)
{
	m_motion_cb = cb;
	m_motion_user_data = user_data;

	int ff = app_accel_init_freefall();
	if (ff) {
		LOG_ERR_CALL_FAILED_INT("app_accel_init_freefall", ff);
	}

	/* Defer the first arm until the main loop and watchdog feeding are
	 * alive: a regression in the interrupt path can then at worst degrade
	 * into bounded 1 Hz events on a running, shell-recoverable device
	 * instead of a boot brick. The HPF needs no settle window here — the
	 * REFERENCE snap in app_accel_set_motion_sensitivity() re-bases it
	 * instantly at arm time. */
	k_work_init_delayable(&m_motion_arm_work, motion_arm_work_handler);
	k_work_schedule(&m_motion_arm_work, K_SECONDS(MOTION_BOOT_ARM_SECONDS));
	return 0;
}
