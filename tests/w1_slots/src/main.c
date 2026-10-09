/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the 1-Wire slot binding (app_w1_slots.c) with the expected
 * slot type of #430 step 3: auto-enroll by sensorN_type, the REPLACED /
 * MISMATCH / ABSENT states after a rebind, and teach / assign refusing a
 * device of another type than the slot expects. The bus is faked in stubs.c.
 */

#include "app_config.h"
#include "app_sensor_types.h"
#include "app_w1_slots.h"

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

void test_bus_set(const uint64_t *dallas, int n_dallas, const uint64_t *mp, int n_mp);

#define DS_A 0x000000aa0001ULL
#define DS_B 0x000000aa0002ULL
#define MP_X 0x000000bb0001ULL
#define MP_Y 0x000000bb0002ULL

#define DALLAS APP_SENSOR_TYPE_DALLAS
#define MP     APP_SENSOR_TYPE_MACHINE_PROBE

static uint8_t *rom_of(struct app_config *c, int slot)
{
	uint8_t *r[] = {c->sensor1_rom, c->sensor2_rom, c->sensor3_rom, c->sensor4_rom};

	return r[slot];
}

static uint8_t *type_of(struct app_config *c, int slot)
{
	uint8_t *t[] = {&c->sensor1_type, &c->sensor2_type, &c->sensor3_type, &c->sensor4_type};

	return t[slot];
}

/* Persisted slot config, as after a boot: runtime and staging agree. */
static void cfg_slot(int slot, uint64_t rom, uint8_t type)
{
	sys_put_be64(rom, rom_of(&g_app_config, slot));
	sys_put_be64(rom, rom_of(app_config(), slot));
	*type_of(&g_app_config, slot) = type;
	*type_of(app_config(), slot) = type;
}

static void bus(const uint64_t *ds, int nds, const uint64_t *mp, int nmp)
{
	test_bus_set(ds, nds, mp, nmp);
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&g_app_config, 0, sizeof(g_app_config));
	memset(app_config(), 0, sizeof(*app_config()));
	bus(NULL, 0, NULL, 0);
	(void)app_w1_slots_rebind();
}

ZTEST_SUITE(w1_slots, NULL, NULL, before, NULL, NULL);

static void assert_slot(int slot, enum app_w1_slot_state state, uint8_t expected, uint8_t detected)
{
	zassert_equal(app_w1_slot_get_state(slot), state, "slot %d state %d", slot + 1,
		      app_w1_slot_get_state(slot));
	zassert_equal(app_w1_slot_get_expected_type(slot), expected, "slot %d expected %u",
		      slot + 1, app_w1_slot_get_expected_type(slot));
	zassert_equal(app_w1_slot_get_detected_type(slot), detected, "slot %d detected %u",
		      slot + 1, app_w1_slot_get_detected_type(slot));
}

/* The slot type enum is the registry type id on every wire field. */
ZTEST(w1_slots, test_slot_type_is_registry_id)
{
	zassert_equal((int)APP_W1_SLOT_DALLAS, (int)APP_SENSOR_TYPE_DALLAS);
	zassert_equal((int)APP_W1_SLOT_MACHINE_PROBE, (int)APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_str_equal(app_w1_slot_type_name(APP_W1_SLOT_MACHINE_PROBE), "machine-probe");
}

/* Fresh unit: every device is enrolled into the lowest free slot and its type
 * written to sensorN_type (runtime + staging). */
ZTEST(w1_slots, test_auto_enroll_writes_expected_type)
{
	const uint64_t ds[] = {DS_A};
	const uint64_t mp[] = {MP_X};

	bus(ds, 1, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 2);

	assert_slot(0, APP_W1_SLOT_STATE_OK, DALLAS, 0);
	assert_slot(1, APP_W1_SLOT_STATE_OK, MP, 0);
	assert_slot(2, APP_W1_SLOT_STATE_NONE, 0, 0);
	zassert_equal(app_w1_slot_get_rom(1), MP_X);
	zassert_equal(*type_of(app_config(), 1), MP, "staging type");
	zassert_equal(app_w1_slot_get_type(1), APP_W1_SLOT_MACHINE_PROBE);
}

/* A slot provisioned for one type (no ROM yet) is skipped by a device of
 * another type, which goes to the next untyped slot. */
ZTEST(w1_slots, test_auto_enroll_skips_slot_expecting_other_type)
{
	const uint64_t mp[] = {MP_X};

	cfg_slot(0, 0, DALLAS);
	bus(NULL, 0, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 1);

	assert_slot(0, APP_W1_SLOT_STATE_ABSENT, DALLAS, 0);
	assert_slot(1, APP_W1_SLOT_STATE_OK, MP, 0);
	zassert_equal(app_w1_slot_get_rom(0), 0, "slot 1 stays untaught");

	/* The matching device later fills the provisioned slot. */
	const uint64_t ds[] = {DS_A};

	bus(ds, 1, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 2);
	assert_slot(0, APP_W1_SLOT_STATE_OK, DALLAS, 0);
	zassert_equal(app_w1_slot_get_rom(0), DS_A);
	zassert_equal(*type_of(app_config(), 0), DALLAS, "staging type follows");
}

/* Every slot provisioned for dallas, a machine probe plugged in: no slot takes
 * it, so the lowest provisioned slot goes to MISMATCH (one slot per foreign
 * device), the rest stay ABSENT. */
ZTEST(w1_slots, test_provisioned_slot_mismatch)
{
	const uint64_t mp[] = {MP_X};

	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		cfg_slot(s, 0, DALLAS);
	}
	bus(NULL, 0, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 0);

	assert_slot(0, APP_W1_SLOT_STATE_MISMATCH, DALLAS, MP);
	assert_slot(1, APP_W1_SLOT_STATE_ABSENT, DALLAS, 0);
	assert_slot(3, APP_W1_SLOT_STATE_ABSENT, DALLAS, 0);
	zassert_false(app_w1_slot_is_present(0));
}

/* A taught machine probe is gone and a DS18B20 is on the bus where no free slot
 * takes a dallas: MISMATCH. With the right probe back, OK again. */
ZTEST(w1_slots, test_taught_slot_mismatch_and_recovery)
{
	const uint64_t ds[] = {DS_A};
	const uint64_t mp[] = {MP_X};

	cfg_slot(0, MP_X, MP);
	for (int s = 1; s < APP_W1_SLOT_COUNT; s++) {
		cfg_slot(s, 0, MP);
	}
	bus(ds, 1, NULL, 0);
	zassert_equal(app_w1_slots_rebind(), 0);
	assert_slot(0, APP_W1_SLOT_STATE_MISMATCH, MP, DALLAS);
	assert_slot(1, APP_W1_SLOT_STATE_ABSENT, MP, 0);
	zassert_equal(app_w1_slot_get_rom(0), MP_X, "ROM kept, never rebound");

	bus(ds, 1, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 1);
	assert_slot(0, APP_W1_SLOT_STATE_OK, MP, 0);
}

/* A taught probe is gone, a foreign-type device appears, but a free untyped
 * slot takes it: enrolled there, the taught slot is just ABSENT. */
ZTEST(w1_slots, test_foreign_device_with_free_slot_is_not_mismatch)
{
	const uint64_t ds[] = {DS_A};

	cfg_slot(0, MP_X, MP);
	bus(ds, 1, NULL, 0);
	zassert_equal(app_w1_slots_rebind(), 1);
	assert_slot(0, APP_W1_SLOT_STATE_ABSENT, MP, 0);
	assert_slot(1, APP_W1_SLOT_STATE_OK, DALLAS, 0);
}

/* A taught probe is gone and another probe of the same type appears: the slot
 * is flagged REPLACED (never silently rebound); the new probe is enrolled into
 * a free slot. */
ZTEST(w1_slots, test_same_type_swap_is_replaced)
{
	const uint64_t mp[] = {MP_Y};

	cfg_slot(0, MP_X, MP);
	bus(NULL, 0, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 1);
	assert_slot(0, APP_W1_SLOT_STATE_REPLACED, MP, 0);
	assert_slot(1, APP_W1_SLOT_STATE_OK, MP, 0);
	zassert_equal(app_w1_slot_get_rom(0), MP_X);
}

/* teach / assign refuse a device whose type differs from the set sensorN_type;
 * clearing the slot drops the expectation and the same device is then taught. */
ZTEST(w1_slots, test_teach_assign_refuse_other_type)
{
	const uint64_t ds[] = {DS_A};
	struct app_w1_scan_entry e;

	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		cfg_slot(s, 0, MP);
	}
	bus(ds, 1, NULL, 0);

	zassert_equal(app_w1_slots_teach(0, &e), -EINVAL);
	zassert_equal(app_w1_slots_assign(0, DS_A), -EINVAL);
	zassert_equal(app_w1_slot_get_rom(0), 0);
	assert_slot(0, APP_W1_SLOT_STATE_MISMATCH, MP, DALLAS);

	zassert_equal(app_w1_slots_clear(0), 0);
	/* The clear drops the expected type, so its rebind auto-enrolls DS_A into
	 * the now untyped slot 1. */
	assert_slot(0, APP_W1_SLOT_STATE_OK, DALLAS, 0);
	zassert_equal(app_w1_slot_get_rom(0), DS_A);
	zassert_equal(*type_of(app_config(), 0), DALLAS, "staging type follows");
}

/* A taught probe swapped for another of its type while no free slot takes the
 * new one: the taught slot is REPLACED and claims it, so a provisioned slot of
 * another type does not see it as a mismatch. A plug-one teach then binds it. */
ZTEST(w1_slots, test_teach_same_type_into_replaced_slot)
{
	const uint64_t mp[] = {MP_Y};
	struct app_w1_scan_entry e;

	cfg_slot(0, MP_X, MP);
	for (int s = 1; s < APP_W1_SLOT_COUNT; s++) {
		cfg_slot(s, 0, DALLAS);
	}
	bus(NULL, 0, mp, 1);
	zassert_equal(app_w1_slots_rebind(), 0);
	assert_slot(0, APP_W1_SLOT_STATE_REPLACED, MP, 0);
	assert_slot(1, APP_W1_SLOT_STATE_ABSENT, DALLAS, 0);

	zassert_equal(app_w1_slots_teach(0, &e), 0);
	zassert_equal(e.serial, MP_Y);
	assert_slot(0, APP_W1_SLOT_STATE_OK, MP, 0);
	zassert_equal(app_w1_slot_get_rom(0), MP_Y);
}

/* The boot scan + rebind runs for a slot provisioned only with sensorN_type,
 * otherwise its probe is never auto-enrolled and a mismatch never seen. */
ZTEST(w1_slots, test_any_taught_counts_provisioned_type)
{
	zassert_false(app_w1_slots_any_taught());

	cfg_slot(2, 0, MP);
	zassert_true(app_w1_slots_any_taught(), "type-only slot");

	cfg_slot(2, 0, 0);
	cfg_slot(1, DS_A, 0);
	zassert_true(app_w1_slots_any_taught(), "ROM-only slot");
}
