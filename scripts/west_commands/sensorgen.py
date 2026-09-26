# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0

"""West command generating the sensor type registry from app_w1_slots.yaml (#430).

Each sensor type (the motherboard on slot 0, 1-Wire types on slots 1..4)
publishes a channel table. This command validates the YAML and generates:

- app_sensor_types.h / app_sensor_types.c: type/channel ids, descriptor tables
  and lookups for the firmware;
- the `// BEGIN GENERATED SENSOR_TYPES` region of decoder/ttn.js, so the
  stateless decoder resolves (type, channel) -> name / unit / scale.

Numbering is append-only: before overwriting, the committed header is parsed and
every type id / channel number it already defines must stay the same (a removed
channel stays listed with `retired: true`).
"""

import argparse
import re
import sys
from pathlib import Path

import yaml
from jinja2 import Environment, FileSystemLoader
from west import log
from west.commands import WestCommand

sys.path.insert(0, str(Path(__file__).parent))
from configen import rewrite_marked_text  # noqa: E402

TEMPLATES_DIR = Path(__file__).parent / "templates"

DECODER_BEGIN = "BEGIN GENERATED SENSOR_TYPES"
DECODER_END = "END GENERATED SENSOR_TYPES"

KINDS = ["threshold", "state", "rate", "none"]
WIRE_TYPES = {"sint32": (-(2**31), 2**31 - 1), "uint32": (0, 2**31 - 1)}
# Usable value range of each history encoding; the top value is the "absent"
# sentinel (INT16_MAX / INT32_MAX / all-ones) and is kept free.
HISTORY_ENCS = {
    "u8": (0, 0xFE),
    "i16": (-0x8000, 0x7FFE),
    "u16": (0, 0xFFFE),
    "i32": (-(2**31), 2**31 - 2),
    "u32": (0, 2**32 - 2),
}

TOP_KEYS = {"version", "max_channels", "max_channels_w1", "pending_caps", "types"}
TYPE_KEYS = {"id", "name", "label", "slot", "w1_family", "channels"}
CHANNEL_KEYS = {
    "ch", "name", "label", "quantity", "unit", "kind", "momentary", "counter",
    "cap", "wire", "history", "range", "liveness", "alarm_only_watchdog", "retired",
}
NAME_RE = re.compile(r"^[a-z][a-z0-9-]*$")


def c_ident(name):
    return re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def load_caps(app_config_yaml):
    """Names of the bool cap_* parameters in app_config.yml."""
    with open(app_config_yaml) as f:
        cfg = yaml.safe_load(f)
    return {p["name"] for p in cfg.get("parameters", [])
            if p.get("name", "").startswith("cap_") and p.get("type") == "bool"}


def _fits(lo, hi, scale, limits):
    a, b = limits
    return a <= round(lo * scale) and round(hi * scale) <= b


def validate(reg, caps):
    """Return a list of error strings (empty = valid)."""
    err = []

    for k in set(reg) - TOP_KEYS:
        err.append(f"unknown top-level key '{k}'")
    for k in ("version", "max_channels", "max_channels_w1", "types"):
        if k not in reg:
            err.append(f"missing top-level key '{k}'")
    if err:
        return err

    pending = set(reg.get("pending_caps") or [])
    for cap in sorted(pending & caps):
        err.append(f"pending cap '{cap}' now exists in app_config.yml — "
                   f"remove it from pending_caps")
    known_caps = caps | pending

    ids, names, families, mb = set(), set(), set(), []
    for t in reg["types"]:
        tname = t.get("name", "?")
        where = f"type '{tname}'"
        for k in set(t) - TYPE_KEYS:
            err.append(f"{where}: unknown key '{k}'")
        tid = t.get("id")
        if not isinstance(tid, int) or not 1 <= tid <= 255:
            err.append(f"{where}: id must be 1..255 (0 = none)")
        elif tid in ids:
            err.append(f"{where}: duplicate id {tid}")
        ids.add(tid)
        if not NAME_RE.match(tname):
            err.append(f"{where}: bad name")
        elif tname in names:
            err.append(f"{where}: duplicate name")
        names.add(tname)
        if "label" not in t:
            err.append(f"{where}: missing label")

        is_mb = "slot" in t
        if is_mb:
            if t["slot"] != 0 or "w1_family" in t:
                err.append(f"{where}: only the motherboard has `slot: 0` (and no w1_family)")
            mb.append(tname)
            limit = reg["max_channels"]
        else:
            fam = t.get("w1_family")
            if not isinstance(fam, int) or not 1 <= fam <= 0xFF:
                err.append(f"{where}: 1-Wire type needs w1_family 0x01..0xFF")
            elif fam in families:
                err.append(f"{where}: duplicate w1_family 0x{fam:02x}")
            families.add(fam)
            limit = reg["max_channels_w1"]

        chans = t.get("channels") or []
        if not chans:
            err.append(f"{where}: no channels")
        if len(chans) > limit:
            err.append(f"{where}: {len(chans)} channels > limit {limit}")
        cnames = set()
        for i, c in enumerate(chans):
            cw = f"{where} ch {c.get('ch', '?')} '{c.get('name', '?')}'"
            for k in set(c) - CHANNEL_KEYS:
                err.append(f"{cw}: unknown key '{k}'")
            if c.get("ch") != i:
                err.append(f"{cw}: channels must be listed in order, ch == position ({i})")
            cn = c.get("name", "")
            if not NAME_RE.match(cn):
                err.append(f"{cw}: bad name")
            elif cn in cnames:
                err.append(f"{cw}: duplicate channel name")
            cnames.add(cn)
            for k in ("label", "quantity", "unit", "kind", "wire"):
                if k not in c:
                    err.append(f"{cw}: missing '{k}'")
            kind = c.get("kind")
            if kind not in KINDS:
                err.append(f"{cw}: kind must be one of {KINDS}")
            if c.get("momentary") and kind != "state":
                err.append(f"{cw}: momentary only applies to kind state")
            if c.get("counter") and kind != "rate":
                err.append(f"{cw}: counter only applies to kind rate")
            if kind == "rate" and not c.get("counter"):
                err.append(f"{cw}: kind rate needs counter: true")
            if c.get("alarm_only_watchdog") and kind != "threshold":
                err.append(f"{cw}: alarm_only_watchdog only applies to kind threshold")
            if c.get("cap") is not None:
                if not is_mb:
                    err.append(f"{cw}: cap only applies to motherboard channels")
                elif c["cap"] not in known_caps:
                    err.append(f"{cw}: unknown capability '{c['cap']}'")

            wire = c.get("wire")
            if wire is not None:
                if (not isinstance(wire, list) or len(wire) != 2 or wire[0] not in WIRE_TYPES
                        or not isinstance(wire[1], (int, float)) or wire[1] <= 0):
                    err.append(f"{cw}: wire must be [sint32|uint32, scale > 0]")
                    wire = None
            hist = c.get("history")
            if hist is not None:
                if (not isinstance(hist, list) or len(hist) != 2 or hist[0] not in HISTORY_ENCS
                        or not isinstance(hist[1], (int, float)) or hist[1] <= 0):
                    err.append(f"{cw}: history must be [{'|'.join(HISTORY_ENCS)}, scale > 0]")
                    hist = None

            rng = c.get("range")
            if kind == "threshold" and rng is None:
                err.append(f"{cw}: kind threshold needs a range")
            if rng is not None:
                if (not isinstance(rng, list) or len(rng) != 2
                        or not all(isinstance(v, (int, float)) for v in rng) or rng[0] >= rng[1]):
                    err.append(f"{cw}: range must be [min, max] with min < max")
                    continue
                lo, hi = rng
                if wire and not _fits(lo, hi, wire[1], WIRE_TYPES[wire[0]]):
                    err.append(f"{cw}: range {rng} x {wire[1]} overflows wire {wire[0]}")
                if hist and not _fits(lo, hi, hist[1], HISTORY_ENCS[hist[0]]):
                    err.append(f"{cw}: range {rng} x {hist[1]} overflows history {hist[0]} "
                               f"(sentinel kept free)")
            elif c.get("counter"):
                # Counters have no range: the store is the raw uint32.
                if wire and wire != ["uint32", 1]:
                    err.append(f"{cw}: counter wire must be [uint32, 1]")
                if hist and hist != ["u32", 1]:
                    err.append(f"{cw}: counter history must be [u32, 1]")

    if len(mb) != 1:
        err.append(f"exactly one motherboard type (`slot: 0`) required, found {mb}")
    return err


def parse_committed_ids(header_text):
    """{symbol: number} of the type / channel ids in a committed header."""
    return {m.group(1): int(m.group(2)) for m in
            re.finditer(r"^\s*(APP_SENSOR_(?:TYPE|CH)_[A-Z0-9_]+) = (\d+),", header_text, re.M)}


def build_model(reg):
    pending = sorted(reg.get("pending_caps") or [])
    types = []
    for t in reg["types"]:
        tid_sym = f"APP_SENSOR_TYPE_{c_ident(t['name'])}"
        chans = []
        for c in t["channels"]:
            wire = c["wire"]
            hist = c.get("history")
            rng = c.get("range")
            flags = [f for f, on in (
                ("APP_SENSOR_F_MOMENTARY", c.get("momentary")),
                ("APP_SENSOR_F_COUNTER", c.get("counter")),
                ("APP_SENSOR_F_LIVENESS", c.get("liveness")),
                ("APP_SENSOR_F_WATCHDOG_ONLY", c.get("alarm_only_watchdog")),
                ("APP_SENSOR_F_RETIRED", c.get("retired")),
                ("APP_SENSOR_F_RANGE", rng is not None),
            ) if on]
            chans.append({
                "ch": c["ch"],
                "name": c["name"],
                "sym": f"APP_SENSOR_CH_{c_ident(t['name'])}_{c_ident(c['name'])}",
                "kind": f"APP_SENSOR_KIND_{c['kind'].upper()}",
                "flags": " | ".join(flags) or "0",
                "wire_type": f"APP_SENSOR_WIRE_{wire[0].upper()}",
                "wire_scale": float(wire[1]),
                "hist_enc": f"APP_SENSOR_HIST_{hist[0].upper()}" if hist else "APP_SENSOR_HIST_NONE",
                "hist_scale": float(hist[1]) if hist else 0.0,
                "range_min": float(rng[0]) if rng else 0.0,
                "range_max": float(rng[1]) if rng else 0.0,
                "cap": c.get("cap") if c.get("cap") not in pending else None,
                "cap_pending": c.get("cap") if c.get("cap") in pending else None,
                # decoder
                "unit": c["unit"],
                "js_kind": c["kind"],
                "js_hist": f'["{hist[0]}", {_js_num(hist[1])}]' if hist else "null",
                "js_scale": _js_num(wire[1]),
                "retired": bool(c.get("retired")),
            })
        types.append({
            "id": t["id"],
            "name": t["name"],
            "sym": tid_sym,
            "ident": c_ident(t["name"]),
            "cname": t["name"].replace("-", "_"),
            "is_mb": "slot" in t,
            "w1_family": t.get("w1_family", 0),
            "channels": chans,
        })
    return {
        "version": reg["version"],
        "max_channels": reg["max_channels"],
        "max_channels_w1": reg["max_channels_w1"],
        "types": types,
        "mb": next(t for t in types if t["is_mb"]),
        "w1_types": [t for t in types if not t["is_mb"]],
        "pending_caps": pending,
    }


def _js_num(v):
    return repr(int(v)) if float(v).is_integer() else repr(float(v))


def render(reg, templates_dir=TEMPLATES_DIR):
    """(header, source, decoder_region) text for a validated registry."""
    env = Environment(loader=FileSystemLoader(str(templates_dir)),
                      trim_blocks=True, lstrip_blocks=True, keep_trailing_newline=True)
    model = build_model(reg)
    return (env.get_template("sensor_types.h.j2").render(**model),
            env.get_template("sensor_types.c.j2").render(**model),
            env.get_template("sensor_types_decoder.js.j2").render(**model).rstrip("\n"))


def guard_no_renumber(new_header, committed_header):
    """Errors for every committed symbol that changed number or vanished."""
    old = parse_committed_ids(committed_header)
    new = parse_committed_ids(new_header)
    err = []
    for sym, num in sorted(old.items()):
        if sym not in new:
            err.append(f"{sym} (= {num}) removed — mark the channel `retired: true` instead")
        elif new[sym] != num:
            err.append(f"{sym} renumbered {num} -> {new[sym]} (numbering is append-only)")
    return err


class Sensorgen(WestCommand):
    def __init__(self):
        super().__init__(
            "sensorgen",
            "generate the sensor type registry from app_w1_slots.yaml",
            __doc__,
            accepts_unknown_args=False,
            requires_workspace=False,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name, help=self.help, description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("yaml_file", type=Path, help="path to app_w1_slots.yaml")
        parser.add_argument("-o", "--output-dir", type=Path, default=None,
                            help="output directory (default: same as the YAML)")
        parser.add_argument("--app-config", type=Path, default=None,
                            help="app_config.yml for the capability check "
                            "(default: <output-dir>/app_config.yml)")
        parser.add_argument("--decoder", type=Path, default=None,
                            help="decoder whose SENSOR_TYPES region is rewritten "
                            "(default: <output-dir>/../decoder/ttn.js)")
        parser.add_argument("--dry-run", action="store_true",
                            help="validate and print, write nothing")
        return parser

    def do_run(self, args, unknown_args):
        yaml_path = args.yaml_file.resolve()
        out = (args.output_dir or yaml_path.parent).resolve()
        app_config = (args.app_config or out / "app_config.yml").resolve()
        decoder = (args.decoder or out.parent / "decoder" / "ttn.js").resolve()

        reg = yaml.safe_load(yaml_path.read_text())
        errors = validate(reg, load_caps(app_config))
        if errors:
            for e in errors:
                log.err(e)
            log.die(f"{yaml_path.name}: {len(errors)} error(s)")

        header, source, region = render(reg)

        header_path = out / "app_sensor_types.h"
        source_path = out / "app_sensor_types.c"
        if header_path.exists():
            errors = guard_no_renumber(header, header_path.read_text())
            if errors:
                for e in errors:
                    log.err(e)
                log.die("sensor type / channel numbering changed")

        if args.dry_run:
            print(header)
            print(source)
            print(region)
            return

        header_path.write_text(header)
        source_path.write_text(source)
        log.inf(f"Generated: {header_path}, {source_path}")
        _clang_format([header_path, source_path])

        if decoder.exists():
            decoder.write_text(rewrite_marked_text(decoder.read_text(), region, "//",
                                                   DECODER_BEGIN, DECODER_END, str(decoder)))
            log.inf(f"Generated decoder region: {decoder}")
        else:
            log.wrn(f"decoder not found ({decoder}); SENSOR_TYPES region not written")


def _clang_format(paths):
    import shutil
    import subprocess

    exe = shutil.which("clang-format")
    if not exe:
        log.wrn("clang-format not found in PATH; generated C left unformatted")
        return
    subprocess.run([exe, "-i", *map(str, paths)], check=True)
