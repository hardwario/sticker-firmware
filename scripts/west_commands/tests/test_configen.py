"""Tests for the configen west command (issue #51, part 2).

Covers the proto/options generator added in #57: idempotence, the append-only
proto_id allocator, the no-renumber guard, YAML validation, and a cross-check
that the firmware proto schema and the ttn.js decoder agree on the wire.

Run: `pytest scripts/west_commands/tests` (from the repo root, inside the venv).
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml as pyyaml

import configen

REPO = Path(__file__).resolve().parents[3]
APP_SRC = REPO / "app" / "src"
YAML = APP_SRC / "app_config.yml"
PROTO = APP_SRC / "app_config.proto"
OPTIONS = APP_SRC / "app_config.options.in"
DECODER = REPO / "app" / "decoder" / "ttn.js"

GEN_FILES = ["app_config.c", "app_config.h", "app_config.proto", "app_config.options.in"]


def _load_config():
    with open(YAML) as f:
        return pyyaml.safe_load(f)


def _run_configen(out_dir):
    """Run the full configen command into out_dir (which must already hold copies
    of the .yml/.proto/.options.in so the marker rewrite has something to edit)."""
    args = argparse.Namespace(
        yaml_file=out_dir / "app_config.yml",
        output_dir=out_dir,
        templates_dir=None,
        proto=None,
        options=None,
        no_proto=False,
        dry_run=False,
    )
    configen.Configen().do_run(args, [])


@pytest.fixture
def workdir(tmp_path):
    for name in ["app_config.yml", "app_config.proto", "app_config.options.in"]:
        shutil.copy(APP_SRC / name, tmp_path / name)
    # Carry the project clang-format style so generated C matches the committed
    # files (clang-format searches upward from the file for .clang-format).
    if (REPO / ".clang-format").exists():
        shutil.copy(REPO / ".clang-format", tmp_path / ".clang-format")
    return tmp_path


# --- pure helpers ---------------------------------------------------------

def test_build_options_lines_matches_committed():
    cfg = _load_config()
    lines = configen.build_options_lines(cfg)
    # The 7 hex Lorawan keys + the 4 per-slot 1-Wire ROM keys get max_length;
    # secret_key is a callback. Order-independent (config declaration order).
    assert sorted(lines) == sorted([
        "AppConfigMessage.Lorawan.deveui max_length:16",
        "AppConfigMessage.Lorawan.joineui max_length:16",
        "AppConfigMessage.Lorawan.nwkkey max_length:32",
        "AppConfigMessage.Lorawan.appkey max_length:32",
        "AppConfigMessage.Lorawan.devaddr max_length:8",
        "AppConfigMessage.Lorawan.nwkskey max_length:32",
        "AppConfigMessage.Lorawan.appskey max_length:32",
        "AppConfigMessage.Application.sensor1_rom max_length:16",
        "AppConfigMessage.Application.sensor2_rom max_length:16",
        "AppConfigMessage.Application.sensor3_rom max_length:16",
        "AppConfigMessage.Application.sensor4_rom max_length:16",
    ])
    assert not any("secret_key" in ln for ln in lines)


def test_build_proto_model_structure():
    model = configen.build_proto_model(_load_config())
    assert model["message"] == "AppConfigMessage"
    root = {f["name"]: f["id"] for f in model["root_fields"]}
    assert root["factory"] == 1
    assert root["secret_key"] == 2
    assert root["lorawan"] == 5 and root["application"] == 6
    subs = {s["name"]: s for s in model["submessages"]}
    assert subs["Lorawan"]["fields"][0]["name"] == "region"
    app = subs["Application"]
    assert 3 in app["reserved"]  # interval_aggreg
    ids = {f["name"]: f["id"] for f in app["fields"]}
    assert ids["history_enable"] == 49 and ids["history_sensors"] == 50
    assert 3 not in ids.values()  # never reused


# --- allocator + guard ----------------------------------------------------

def test_allocator_appends_next_free():
    cfg = _load_config()
    # ruamel doc only needed for write-back; reuse the same yaml on disk.
    from ruamel.yaml import YAML as RT
    rt = RT()
    with open(YAML) as f:
        rt_doc = rt.load(f)

    from ruamel.yaml.comments import CommentedMap
    cfg["parameters"].append({"name": "telemetry_split", "type": "bool",
                              "proto_group": "application"})
    rt_new = CommentedMap()
    rt_new["name"] = "telemetry_split"
    rt_new["proto_group"] = "application"
    rt_new["type"] = "bool"
    rt_doc["parameters"].append(rt_new)

    # Expected id = next free in the application group (1 + current max),
    # computed from the YAML so this doesn't break when fields are added.
    app_ids = [p["proto_id"] for p in cfg["parameters"]
               if p.get("proto_group") == "application" and "proto_id" in p
               and p["name"] != "telemetry_split"]
    expected = max(app_ids) + 1

    assigned = configen.allocate_proto_ids(cfg, rt_doc)
    assert ("telemetry_split", expected) in assigned
    got = next(p for p in cfg["parameters"] if p["name"] == "telemetry_split")
    assert got["proto_id"] == expected


def test_guard_rejects_renumbering():
    cfg = _load_config()
    region = next(p for p in cfg["parameters"] if p["name"] == "lrw_region")
    region["proto_id"] = 99  # differs from the locked 1 in the .proto
    with pytest.raises(SystemExit):
        configen.guard_no_renumber(cfg, PROTO)


def test_guard_accepts_unchanged():
    configen.guard_no_renumber(_load_config(), PROTO)  # must not raise


def test_missing_proto_group_is_rejected():
    cfg = _load_config()
    cfg["parameters"].append({"name": "bad", "type": "bool"})
    from ruamel.yaml import YAML as RT
    rt = RT()
    with open(YAML) as f:
        rt_doc = rt.load(f)
    with pytest.raises(SystemExit):
        configen.allocate_proto_ids(cfg, rt_doc)


def test_duplicate_proto_id_is_rejected():
    cfg = _load_config()
    # force a collision in the application namespace
    next(p for p in cfg["parameters"] if p["name"] == "calibration")["proto_id"] = 2
    from ruamel.yaml import YAML as RT
    rt = RT()
    with open(YAML) as f:
        rt_doc = rt.load(f)
    with pytest.raises(SystemExit):
        configen.allocate_proto_ids(cfg, rt_doc)


# --- full run -------------------------------------------------------------

def test_generation_is_idempotent(workdir):
    _run_configen(workdir)
    first = {n: (workdir / n).read_text() for n in GEN_FILES}
    _run_configen(workdir)
    second = {n: (workdir / n).read_text() for n in GEN_FILES}
    assert first == second


def test_proto_region_preserves_all_field_numbers(workdir):
    _run_configen(workdir)
    committed = configen._parse_proto_field_ids(PROTO.read_text())
    regenerated = configen._parse_proto_field_ids((workdir / "app_config.proto").read_text())
    # every field present in the committed proto keeps its number
    for name, num in committed.items():
        assert regenerated.get(name) == num


@pytest.mark.skipif(shutil.which("clang-format") is None,
                    reason="clang-format not available")
def test_generated_c_matches_committed(workdir):
    _run_configen(workdir)
    for name in ["app_config.c", "app_config.h"]:
        assert (workdir / name).read_text() == (APP_SRC / name).read_text(), name


def test_migration_preserves_factory_fields(workdir):
    """Issue #87: h_commit must restore every preserve_on_migration parameter
    after the defaults reset, and must not restore anything else."""
    _run_configen(workdir)
    generated = (workdir / "app_config.c").read_text()
    cfg = _load_config()

    preserved = [p for p in cfg["parameters"] if p.get("preserve_on_migration")]
    # The whole point of #87: identity/credentials are flagged in the YAML.
    assert {p["name"] for p in preserved} >= {
        "secret_key", "serial_number", "nonce_counter",
        "lrw_deveui", "lrw_joineui", "lrw_appkey", "lrw_nwkkey",
        "lrw_devaddr", "lrw_nwkskey", "lrw_appskey",
    }

    for p in cfg["parameters"]:
        if p.get("preserve_on_migration"):
            if p["type"] in ("bytes", "string"):
                marker = f"memcpy(m_app_config.{p['name']}, stored.{p['name']}"
            else:
                marker = f"m_app_config.{p['name']} = stored.{p['name']};"
            assert marker in generated, f"{p['name']} not restored on migration"
        else:
            assert f"stored.{p['name']}" not in generated, \
                f"{p['name']} restored but not flagged preserve_on_migration"

    # The migration must be persisted exactly once, from init, not from h_commit.
    assert "settings_save_subtree(SETTINGS_PFX)" in generated
    assert generated.count("m_app_config_migrated = true") == 1


def test_h_commit_clamps_loaded_values(workdir):
    """Issue #91: h_commit must clamp out-of-range NVS values so a corrupted
    record can't break the device (interval=0 hang, out-of-range enum)."""
    _run_configen(workdir)
    generated = (workdir / "app_config.c").read_text()

    # interval_report (min 60, max 86400) clamps both ways.
    assert "if (m_app_config.interval_report < 60) {" in generated
    assert "if (m_app_config.interval_report > 86400) {" in generated
    # interval_sample is zero_allowed: the low clamp must spare 0.
    assert ("if (m_app_config.interval_sample < 5 && "
            "m_app_config.interval_sample != 0) {") in generated
    # enums clamp to their valid range (lrw_activation 0..1).
    assert "(int)m_app_config.lrw_activation > 1) {" in generated
    # float ranges keep the f suffix.
    assert "if (m_app_config.temperature_corr > 5.0f) {" in generated


# --- proto <-> decoder cross-check ---------------------------------------

COMMAND_VECTORS = {
    # set_param: lorawan.adr=1, application.interval_report=120, temperature_corr=2.5
    # (the fixed threshold alarm keys were retired in the dynamic-alarms migration).
    "set_param": "0801120e0a02180112082078ad0200002040",
    "get_param": "08021a070a010312020407",
    "reboot": "08083a00",
}


def _compile_proto(tmp_path):
    from grpc_tools import protoc
    rc = protoc.main([
        "protoc", f"-I{APP_SRC}", f"--python_out={tmp_path}", str(PROTO),
    ])
    assert rc == 0, "protoc failed"
    sys.path.insert(0, str(tmp_path))
    import app_config_pb2
    return app_config_pb2


def _decode_with_node(hex_str):
    js = (
        "const fs=require('fs'),vm=require('vm');"
        "const ctx={Buffer,console};vm.createContext(ctx);"
        f"vm.runInContext(fs.readFileSync({str(DECODER)!r},'utf8'),ctx);"
        f"process.stdout.write(JSON.stringify("
        f"ctx.decodeDownlink({{bytes:Buffer.from('{hex_str}','hex'),fPort:85}}).data));"
    )
    out = subprocess.check_output(["node", "-e", js], text=True)
    import json
    return json.loads(out)


@pytest.mark.skipif(shutil.which("node") is None, reason="node not available")
def test_proto_and_decoder_agree(tmp_path):
    pb = _compile_proto(tmp_path)

    # set_param: python protobuf and ttn.js must read the same fields
    msg = pb.Command.FromString(bytes.fromhex(COMMAND_VECTORS["set_param"]))
    assert msg.seq == 1
    assert msg.set_param.lorawan.adr is True
    assert msg.set_param.application.interval_report == 120
    assert abs(msg.set_param.application.temperature_corr - 2.5) < 1e-6

    js = _decode_with_node(COMMAND_VECTORS["set_param"])
    assert js["seq"] == 1
    assert js["set_param"]["application"]["interval_report"] == 120
    assert abs(js["set_param"]["application"]["temperature_corr"] - 2.5) < 1e-6

    # get_param field lists
    msg = pb.Command.FromString(bytes.fromhex(COMMAND_VECTORS["get_param"]))
    assert list(msg.get_param.lorawan_field) == [3]
    assert list(msg.get_param.application_field) == [4, 7]
    js = _decode_with_node(COMMAND_VECTORS["get_param"])
    assert js["get_param"]["lorawan_field"] == [3]
    assert js["get_param"]["application_field"] == [4, 7]
