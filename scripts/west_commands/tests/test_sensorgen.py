"""Tests for the sensorgen west command (#430): the sensor type registry
app_w1_slots.yaml, its validation, the append-only numbering guard, and that the
committed generated files (app_sensor_types.{c,h}, the ttn.js SENSOR_TYPES
region) are in sync with the YAML.

Run: `pytest scripts/west_commands/tests` (from the repo root, inside the venv).
"""
import argparse
import copy
import shutil
from pathlib import Path

import pytest
import yaml as pyyaml

import sensorgen

REPO = Path(__file__).resolve().parents[3]
APP_SRC = REPO / "app" / "src"
REGISTRY = APP_SRC / "app_w1_slots.yaml"
APP_CONFIG = APP_SRC / "app_config.yml"
DECODER = REPO / "app" / "decoder" / "ttn.js"


def _registry():
    return pyyaml.safe_load(REGISTRY.read_text())


def _caps():
    return sensorgen.load_caps(APP_CONFIG)


def _errors(reg):
    return sensorgen.validate(reg, _caps())


def _run(workdir):
    sensorgen.Sensorgen().do_run(argparse.Namespace(
        yaml_file=workdir / "app_w1_slots.yaml",
        output_dir=workdir,
        app_config=workdir / "app_config.yml",
        decoder=workdir / "ttn.js",
        dry_run=False,
    ), [])


@pytest.fixture
def workdir(tmp_path):
    for name in ["app_w1_slots.yaml", "app_config.yml", "app_sensor_types.h", "app_sensor_types.c"]:
        shutil.copy(APP_SRC / name, tmp_path / name)
    shutil.copy(DECODER, tmp_path / "ttn.js")
    if (REPO / ".clang-format").exists():
        shutil.copy(REPO / ".clang-format", tmp_path / ".clang-format")
    return tmp_path


def _type(reg, name):
    return next(t for t in reg["types"] if t["name"] == name)


# --- committed registry ---------------------------------------------------

def test_committed_registry_is_valid():
    assert _errors(_registry()) == []


def test_committed_registry_shape():
    reg = _registry()
    mb = _type(reg, "motherboard")
    assert mb["id"] == 1 and mb["slot"] == 0
    assert _type(reg, "dallas")["id"] == 2
    assert _type(reg, "machine-probe")["id"] == 3
    # The TMP112 is the second temperature on the machine-probe (#430 motivation).
    mp = [c["name"] for c in _type(reg, "machine-probe")["channels"]]
    assert mp[:3] == ["temperature", "humidity", "temperature-aux"]


@pytest.mark.skipif(shutil.which("clang-format") is None, reason="clang-format not available")
def test_generated_c_matches_committed(workdir):
    _run(workdir)
    for name in ["app_sensor_types.h", "app_sensor_types.c"]:
        assert (workdir / name).read_text() == (APP_SRC / name).read_text(), \
            f"{name} is stale — run `west sensorgen app/src/app_w1_slots.yaml`"


def test_generated_decoder_region_matches_committed(workdir):
    _run(workdir)
    assert (workdir / "ttn.js").read_text() == DECODER.read_text(), \
        "ttn.js SENSOR_TYPES region is stale — run `west sensorgen app/src/app_w1_slots.yaml`"


def test_generation_is_idempotent(workdir):
    _run(workdir)
    first = {n: (workdir / n).read_text() for n in ["app_sensor_types.h", "app_sensor_types.c", "ttn.js"]}
    _run(workdir)
    for n, text in first.items():
        assert (workdir / n).read_text() == text


# --- append-only numbering ------------------------------------------------

def _header(reg):
    return sensorgen.render(reg)[0]


def test_guard_accepts_unchanged_and_appended():
    reg = _registry()
    committed = _header(reg)
    grown = copy.deepcopy(reg)
    ch = _type(grown, "dallas")["channels"]
    ch.append(dict(ch[0], ch=1, name="temperature-2", liveness=False))
    assert sensorgen.guard_no_renumber(_header(reg), committed) == []
    assert sensorgen.guard_no_renumber(_header(grown), committed) == []


def test_guard_rejects_renumbered_channel():
    reg = _registry()
    committed = _header(reg)
    swapped = copy.deepcopy(reg)
    ch = _type(swapped, "machine-probe")["channels"]
    ch[0]["name"], ch[2]["name"] = ch[2]["name"], ch[0]["name"]
    errors = sensorgen.guard_no_renumber(_header(swapped), committed)
    assert any("MACHINE_PROBE_TEMPERATURE renumbered 0 -> 2" in e for e in errors)


def test_guard_rejects_removed_channel_and_type_id_change():
    reg = _registry()
    committed = _header(reg)
    removed = copy.deepcopy(reg)
    _type(removed, "machine-probe")["channels"].pop()
    _type(removed, "dallas")["id"] = 9
    errors = sensorgen.guard_no_renumber(_header(removed), committed)
    assert any("MACHINE_PROBE_ACCEL_Z" in e and "retired" in e for e in errors)
    assert any("APP_SENSOR_TYPE_DALLAS renumbered 2 -> 9" in e for e in errors)


def test_retired_channel_keeps_its_number():
    reg = _registry()
    committed = _header(reg)
    retired = copy.deepcopy(reg)
    _type(retired, "machine-probe")["channels"][8]["retired"] = True
    assert _errors(retired) == []
    assert sensorgen.guard_no_renumber(_header(retired), committed) == []
    assert "APP_SENSOR_F_RETIRED" in sensorgen.render(retired)[1]


# --- validation -----------------------------------------------------------

def _mutated(fn):
    reg = copy.deepcopy(_registry())
    fn(reg)
    return _errors(reg)


def _has(errors, text):
    return any(text in e for e in errors), errors


def test_range_overflowing_history_is_rejected():
    # 0.001 degC over [-200, 850] does not fit i16 -> needs i32 (#430 D2 addendum).
    def f(reg):
        c = _type(reg, "dallas")["channels"][0]
        c.update(wire=["sint32", 1000], history=["i16", 1000], range=[-200.0, 850.0])
    ok, e = _has(_mutated(f), "overflows history i16")
    assert ok, e


def test_high_resolution_channel_with_i32_is_accepted():
    def f(reg):
        c = _type(reg, "dallas")["channels"][0]
        c.update(wire=["sint32", 1000], history=["i32", 1000], range=[-200.0, 850.0])
    assert _mutated(f) == []


def test_history_sentinel_is_kept_free():
    # humidity 0..127.5 %RH at x2 = 255 would collide with the u8 sentinel 0xFF.
    def f(reg):
        _type(reg, "motherboard")["channels"][1]["range"] = [0.0, 127.5]
    ok, e = _has(_mutated(f), "overflows history u8")
    assert ok, e


@pytest.mark.parametrize("mutate, message", [
    (lambda r: _type(r, "dallas").update(id=1), "duplicate id 1"),
    (lambda r: _type(r, "dallas").update(id=0), "id must be 1..255"),
    (lambda r: _type(r, "dallas").update(w1_family=0x19), "duplicate w1_family"),
    (lambda r: _type(r, "dallas").update(slot=0), "only the motherboard"),
    (lambda r: _type(r, "dallas").update(colour="red"), "unknown key 'colour'"),
    (lambda r: r.update(extra=1), "unknown top-level key 'extra'"),
    (lambda r: _type(r, "machine-probe")["channels"].pop(0), "ch == position"),
    (lambda r: _type(r, "machine-probe")["channels"][1].update(name="temperature"),
     "duplicate channel name"),
    (lambda r: _type(r, "machine-probe")["channels"][0].pop("range"), "needs a range"),
    (lambda r: _type(r, "machine-probe")["channels"][0].pop("unit"), "missing 'unit'"),
    (lambda r: _type(r, "machine-probe")["channels"][0].update(kind="analog"), "kind must be"),
    (lambda r: _type(r, "machine-probe")["channels"][0].update(momentary=True),
     "momentary only applies"),
    (lambda r: _type(r, "machine-probe")["channels"][0].update(cap="cap_barometer"),
     "cap only applies to motherboard"),
    (lambda r: _type(r, "motherboard")["channels"][2].update(cap="cap_nonexistent"),
     "unknown capability"),
    (lambda r: _type(r, "motherboard")["channels"][6].update(wire=["sint32", 10]),
     "counter wire must be"),
    (lambda r: _type(r, "motherboard")["channels"][6].pop("counter"), "needs counter: true"),
    (lambda r: _type(r, "motherboard")["channels"][0].update(wire=["float", 1]), "wire must be"),
    (lambda r: _type(r, "motherboard")["channels"][0].update(history=["i8", 1]),
     "history must be"),
    (lambda r: _type(r, "motherboard")["channels"][0].update(range=[5.0, 1.0]), "min < max"),
    (lambda r: r.update(max_channels_w1=5), "9 channels > limit 5"),
    (lambda r: r["types"].append(dict(_type(r, "motherboard"), id=99, name="board-2")),
     "exactly one motherboard"),
])
def test_invalid_registry_is_rejected(mutate, message):
    ok, e = _has(_mutated(mutate), message)
    assert ok, e


def test_pending_cap_must_be_removed_once_it_exists():
    def f(reg):
        reg["pending_caps"] = ["cap_barometer"]
    ok, e = _has(_mutated(f), "pending cap 'cap_barometer' now exists")
    assert ok, e


def test_every_motherboard_cap_exists_or_is_pending():
    reg = _registry()
    known = _caps() | set(reg.get("pending_caps") or [])
    for c in _type(reg, "motherboard")["channels"]:
        if "cap" in c:
            assert c["cap"] in known, c["name"]
