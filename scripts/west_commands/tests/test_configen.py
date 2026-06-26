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
        cfg = pyyaml.safe_load(f)
    # Mirror do_run: derive the internal access flags (dump/no_shell/...) from the
    # readable/writable transport lists before the model builders consume them.
    configen.normalize_access(cfg)
    return cfg


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
        decoder=out_dir / "ttn.js",
        app_cmd=out_dir / "app_cmd.c",
        dry_run=False,
    )
    configen.Configen().do_run(args, [])


@pytest.fixture
def workdir(tmp_path):
    for name in ["app_config.yml", "app_config.proto", "app_config.options.in"]:
        shutil.copy(APP_SRC / name, tmp_path / name)
    # The decoder's command-map region and app_cmd.c's dispatch region are
    # rewritten in place; copy them so the run has marked regions to edit.
    shutil.copy(DECODER, tmp_path / "ttn.js")
    shutil.copy(APP_SRC / "app_cmd.c", tmp_path / "app_cmd.c")
    # Carry the project clang-format style so generated C matches the committed
    # files (clang-format searches upward from the file for .clang-format).
    if (REPO / ".clang-format").exists():
        shutil.copy(REPO / ".clang-format", tmp_path / ".clang-format")
    return tmp_path


# --- pure helpers ---------------------------------------------------------

def test_build_options_lines_matches_committed():
    cfg = _load_config()
    lines = configen.build_options_lines(cfg)
    # The 7 LoRaWAN keys + the 4 per-slot 1-Wire ROM keys + the 16 packed alarm
    # slots are native bytes with nanopb fixed_length (plain pb_byte_t[size], half
    # the old hex wire size); secret_key is a callback. Order-independent.
    assert sorted(lines) == sorted([
        "AppConfigMessage.Lorawan.deveui max_size:8 fixed_length:true",
        "AppConfigMessage.Lorawan.joineui max_size:8 fixed_length:true",
        "AppConfigMessage.Lorawan.nwkkey max_size:16 fixed_length:true",
        "AppConfigMessage.Lorawan.appkey max_size:16 fixed_length:true",
        "AppConfigMessage.Lorawan.devaddr max_size:4 fixed_length:true",
        "AppConfigMessage.Lorawan.nwkskey max_size:16 fixed_length:true",
        "AppConfigMessage.Lorawan.appskey max_size:16 fixed_length:true",
        "AppConfigMessage.Sensors.sensor1_rom max_size:8 fixed_length:true",
        "AppConfigMessage.Sensors.sensor2_rom max_size:8 fixed_length:true",
        "AppConfigMessage.Sensors.sensor3_rom max_size:8 fixed_length:true",
        "AppConfigMessage.Sensors.sensor4_rom max_size:8 fixed_length:true",
    ] + [
        f"AppConfigMessage.Alarms.alarm_{i} max_size:17 fixed_length:true" for i in range(16)
    ])
    assert not any("secret_key" in ln for ln in lines)


def test_no_shell_omits_shell_command():
    """A `no_shell` param keeps NVS/proto/ingest but generates no shell get/set
    command (used by the packed alarm slots to stay within the FLASH budget)."""
    c = (APP_SRC / "app_config.c").read_text()
    ingest = (APP_SRC / "app_config_ingest.c").read_text()
    # alarm_0 is declared no_shell: no shell function or sub-command entry...
    assert "cmd_alarm_0(" not in c
    assert "print_alarm_0(" not in c
    # ...but it still round-trips through SetParam/GetParam ingest.
    assert "config->alarm_0" in ingest
    # A normal param in the same group keeps its shell command.
    assert "cmd_alarm_limit(" in c


def test_normalize_access_derives_internal_flags():
    """readable/writable transport lists are the source of truth; configen derives
    the legacy dump/dump_nfc_only/no_shell/readonly/proto_callback flags from them."""
    cfg = {"parameters": [
        # secret: shell-only read+write, root bytes -> off-wire callback, no dump
        {"name": "secret_key", "proto_group": "root", "type": "bytes",
         "readable": ["shell"], "writable": ["shell"]},
        # claim: readable everywhere, write-once shell only; root bytes -> callback
        {"name": "claim_token", "proto_group": "root", "type": "bytes",
         "readable": ["shell", "nfc", "lrw"], "writable": ["shell"]},
        # key: NFC-readable only (never over LoRaWAN), writable everywhere
        {"name": "lrw_nwkkey", "proto_group": "lorawan", "type": "bytes",
         "readable": ["shell", "nfc"]},
        # packed slot: no shell entry, air read/write only
        {"name": "alarm_0", "proto_group": "alarms", "type": "bytes",
         "readable": ["nfc", "lrw"], "writable": ["nfc", "lrw"]},
        # plain param: lists omitted -> all transports, normal flags
        {"name": "interval_report", "proto_group": "application", "type": "int"},
    ]}
    configen.normalize_access(cfg)
    by = {p["name"]: p for p in cfg["parameters"]}

    assert by["secret_key"]["dump"] is False
    assert by["secret_key"]["proto_callback"] is True
    assert "no_shell" not in by["secret_key"] and "readonly" not in by["secret_key"]

    assert by["claim_token"]["proto_callback"] is True  # root bytes -> off-wire
    assert by["claim_token"]["dump"] is True             # readable over the air
    assert "readonly" not in by["claim_token"]           # shell is writable

    assert by["lrw_nwkkey"]["dump_nfc_only"] is True
    assert "proto_callback" not in by["lrw_nwkkey"]      # submessage bytes = native

    assert by["alarm_0"]["no_shell"] is True
    assert "proto_callback" not in by["alarm_0"]

    assert by["interval_report"]["dump"] is True
    assert "no_shell" not in by["interval_report"]


def test_normalize_access_rejects_bad_transport():
    cfg = {"parameters": [{"name": "x", "proto_group": "root", "type": "bool",
                           "readable": ["shell", "ble"]}]}
    with pytest.raises(SystemExit):
        configen.normalize_access(cfg)


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
    assert app["reserved"] == []  # #166 dropped the leftover gaps
    ids = {f["name"]: f["id"] for f in app["fields"]}
    assert ids["history_enable"] == 4 and ids["history_sensors"] == 5
    assert ids["battery_level"] == 6  # low-battery alarm threshold (#210)
    assert sorted(ids.values()) == [1, 2, 3, 4, 5, 6]  # contiguous


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
    for name in ["app_config.c", "app_config.h", "app_config_ingest.c"]:
        assert (workdir / name).read_text() == (APP_SRC / name).read_text(), name


def test_migration_preserves_factory_fields(workdir):
    """Issue #87/#108: h_commit must restore every preserve_on_reset parameter
    after the defaults reset, and must not restore anything else."""
    _run_configen(workdir)
    generated = (workdir / "app_config.c").read_text()
    cfg = _load_config()

    preserved = [p for p in cfg["parameters"] if p.get("preserve_on_reset")]
    # The whole point of #87: identity/credentials are flagged in the YAML.
    assert {p["name"] for p in preserved} >= {
        "secret_key", "serial_number", "nonce_counter",
        "lrw_deveui", "lrw_joineui", "lrw_appkey", "lrw_nwkkey",
        "lrw_devaddr", "lrw_nwkskey", "lrw_appskey",
    }

    for p in cfg["parameters"]:
        if p.get("preserve_on_reset"):
            if p["type"] in ("bytes", "string"):
                marker = f"memcpy(m_app_config.{p['name']}, stored.{p['name']}"
            else:
                marker = f"m_app_config.{p['name']} = stored.{p['name']};"
            assert marker in generated, f"{p['name']} not restored on migration"
        else:
            assert f"stored.{p['name']}" not in generated, \
                f"{p['name']} restored but not flagged preserve_on_reset"

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
    # (no float config params remain after the temperature-correction removal —
    # the float-clamp branch is unexercised by any parameter.)


# --- proto <-> decoder cross-check ---------------------------------------

COMMAND_VECTORS = {
    # set_param: lorawan.adr=1, application.interval_report=120,
    # sensors.cap_barometer=true (a sensors-submessage field; temperature-corr
    # params were removed, threshold alarm keys retired with dynamic-alarms).
    # Field numbers are the contiguous post-#166 ids (adr=4, interval_report=3,
    # cap_barometer=6).
    "set_param": "0801120c0a0220011202187822023001",
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


def _encode_with_node(data):
    import json
    js = (
        "const fs=require('fs'),vm=require('vm');"
        "const ctx={Buffer,console};vm.createContext(ctx);"
        f"vm.runInContext(fs.readFileSync({str(DECODER)!r},'utf8'),ctx);"
        f"const r=ctx.encodeDownlink({{data:{json.dumps(data)}}});"
        "if(r.errors&&r.errors.length)throw new Error(r.errors.join(','));"
        "process.stdout.write(Buffer.from(r.bytes).toString('hex'));"
    )
    return subprocess.check_output(["node", "-e", js], text=True)


@pytest.mark.skipif(shutil.which("node") is None, reason="node not available")
def test_bytes_fields_native_roundtrip(tmp_path):
    """Config `bytes` params go on the wire as native protobuf bytes (not a hex
    string), so the payload is half the size. The ttn.js decoder keeps its hex
    contract (hexes the raw bytes for output; accepts hex on encode). Verify both
    directions agree between python-protobuf and ttn.js."""
    pb = _compile_proto(tmp_path)

    # python encodes raw bytes -> ttn.js decode must present them as hex
    cmd = pb.Command()
    cmd.seq = 9
    cmd.set_param.lorawan.deveui = bytes.fromhex("70b3d5470a0b0c0d")
    cmd.set_param.sensors.sensor1_rom = bytes.fromhex("28000011223344a5")
    wire = cmd.SerializeToString()
    # native bytes: deveui occupies 8 payload bytes, not 16 hex chars
    assert b"70b3d5470a0b0c0d" not in wire  # not ASCII hex on the wire
    js = _decode_with_node(wire.hex())
    assert js["set_param"]["lorawan"]["deveui"] == "70b3d5470a0b0c0d"
    assert js["set_param"]["sensors"]["sensor1_rom"] == "28000011223344a5"

    # ttn.js encodes hex -> python protobuf must read the raw bytes
    enc = _encode_with_node({
        "command": "set_param",
        "set_param": {"lorawan": {"deveui": "70b3d5470a0b0c0d"}},
    })
    msg = pb.Command.FromString(bytes.fromhex(enc))
    assert msg.set_param.lorawan.deveui == bytes.fromhex("70b3d5470a0b0c0d")


@pytest.mark.skipif(shutil.which("node") is None, reason="node not available")
def test_proto_and_decoder_agree(tmp_path):
    pb = _compile_proto(tmp_path)

    # set_param: python protobuf and ttn.js must read the same fields
    msg = pb.Command.FromString(bytes.fromhex(COMMAND_VECTORS["set_param"]))
    assert msg.seq == 1
    assert msg.set_param.lorawan.adr is True
    assert msg.set_param.application.interval_report == 120
    assert msg.set_param.sensors.cap_barometer is True

    js = _decode_with_node(COMMAND_VECTORS["set_param"])
    assert js["seq"] == 1
    assert js["set_param"]["application"]["interval_report"] == 120
    assert js["set_param"]["sensors"]["cap_barometer"] == 1

    # get_param field lists
    msg = pb.Command.FromString(bytes.fromhex(COMMAND_VECTORS["get_param"]))
    assert list(msg.get_param.lorawan_field) == [3]
    assert list(msg.get_param.application_field) == [4, 7]
    js = _decode_with_node(COMMAND_VECTORS["get_param"])
    assert js["get_param"]["lorawan_field"] == [3]
    assert js["get_param"]["application_field"] == [4, 7]


def _decode_uplink_with_node(hex_str, fport=85):
    js = (
        "const fs=require('fs'),vm=require('vm');"
        "const ctx={Buffer,console};vm.createContext(ctx);"
        f"vm.runInContext(fs.readFileSync({str(DECODER)!r},'utf8'),ctx);"
        f"process.stdout.write(JSON.stringify("
        f"ctx.decodeUplink({{bytes:Buffer.from('{hex_str}','hex'),fPort:{fport}}}).data));"
    )
    out = subprocess.check_output(["node", "-e", js], text=True)
    import json
    return json.loads(out)


# --- commands codegen -----------------------------------------------------

def test_build_commands_model_shape():
    model = configen.build_commands_model(_load_config())
    assert model["message"] == "Command"
    assert model["response_message"] == "Response"
    by_name = {c["name"]: c for c in model["commands"]}
    # wire identity + routing carried from the YAML
    assert by_name["w1_scan"]["proto_id"] == 14
    assert by_name["w1_scan"]["tag"] == "Command_w1_scan_tag"
    assert by_name["w1_scan"]["handler"] == "app_cmd_handle_w1_scan"
    # action vs handler kinds
    assert by_name["reboot"]["kind"] == "action"
    assert by_name["reboot"]["action"] == "REBOOT"
    assert by_name["set_param"]["kind"] == "handler"
    # transport gating + no-immediate-response flags
    assert by_name["force_send"]["lrw_only"] is True
    assert by_name["force_send"]["emits_response"] is False
    assert by_name["clock_sync"]["emits_response"] is False  # info_deferred
    assert by_name["set_param"]["lrw_only"] is False
    assert by_name["set_param"]["emits_response"] is True
    # proto-id order for the oneof
    assert [c["proto_id"] for c in model["commands_by_id"]] == \
        sorted(c["proto_id"] for c in model["commands"])


def test_build_commands_model_rejects_duplicates():
    cfg = _load_config()
    cfg["commands"]["list"].append(
        {"name": "dupe", "proto_id": 14, "body": "X", "kind": "action",
         "action": "REBOOT", "response": "ack"})
    with pytest.raises(SystemExit):
        configen.build_commands_model(cfg)


def test_generated_decoder_command_region_matches_committed(workdir):
    """The _CMD_NAMES region the generator emits must equal what is committed in
    ttn.js (locks the name<->tag map against hand-edits / YAML drift)."""
    _run_configen(workdir)
    assert (workdir / "ttn.js").read_text() == DECODER.read_text()


@pytest.mark.skipif(shutil.which("clang-format") is None,
                    reason="clang-format not available")
def test_generated_dispatch_region_matches_committed(workdir):
    """The app_cmd_dispatch() switch the generator emits must equal what is
    committed in app_cmd.c (locks routing against YAML drift)."""
    _run_configen(workdir)
    assert (workdir / "app_cmd.c").read_text() == (APP_SRC / "app_cmd.c").read_text()


def test_generated_proto_command_oneof_matches_committed(workdir):
    """The generated Command oneof region must equal what is committed in the
    .proto (locks the command wire ids alongside the config region)."""
    _run_configen(workdir)
    assert (workdir / "app_config.proto").read_text() == PROTO.read_text()


def test_command_renumber_is_rejected(workdir):
    """Changing a command's proto_id away from the committed .proto fails the run
    (deployed downlinks / NFC tags encode the tag)."""
    _run_configen(workdir)  # seed the committed ids into workdir proto
    import ruamel.yaml
    rt = ruamel.yaml.YAML()
    with open(workdir / "app_config.yml") as f:
        doc = rt.load(f)
    for c in doc["commands"]["list"]:
        if c["name"] == "w1_scan":
            c["proto_id"] = 20  # was 14
    with open(workdir / "app_config.yml", "w") as f:
        rt.dump(doc, f)
    with pytest.raises(SystemExit):
        _run_configen(workdir)


def test_dispatch_template_routes_every_command():
    """Every command in the model appears in the rendered dispatch switch, with
    LRW-only commands guarded and action commands setting their action."""
    import jinja2
    model = configen.build_commands_model(_load_config())
    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(configen.TEMPLATES_DIR),
        trim_blocks=True, lstrip_blocks=True, keep_trailing_newline=True)
    out = env.get_template("commands_dispatch.c.j2").render(**model)
    for c in model["commands"]:
        assert f"case {c['tag']}:" in out, c["name"]
        if c["kind"] == "action":
            assert f"APP_CMD_ACTION_{c['action']}" in out, c["name"]
        else:
            assert f"{c['handler']}(tp, cmd, resp, action);" in out, c["name"]
        if c["lrw_only"]:
            # the guard precedes this command's handler/action body
            head = out.split(f"case {c['tag']}:", 1)[1]
            assert "tp != APP_CMD_TRANSPORT_LRW" in head.split("break;", 1)[0]
