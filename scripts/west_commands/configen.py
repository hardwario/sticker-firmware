# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0

"""West command for generating configuration modules from YAML files.

This command generates C source and header files with support for:
- Zephyr Settings subsystem
- Zephyr Shell commands
- Configuration structure in the header file

See `configen-template.yml` in this directory for a complete example
with all supported features and detailed comments.

Supported types:
- bool: boolean (true/false)
- int8, int16, int32, int64, int: signed integers
- uint8, uint16, uint32, uint64, uint: unsigned integers
- float, double: floating point
- string: fixed-size string buffer
- bytes: byte array (hex encoded)
- enum: enumerated values

Parameter options:
- default: default value
- min/max: range constraints for numeric types
- size: byte array size (for bytes type)
- minlen/maxlen: length constraints for string type
- array: array size for numeric types
- hidden: hide from shell 'show' command
- readonly: prevent shell modification
- no_shell: skip shell get/set command generation entirely (param still has
  NVS/proto/ingest; use for bulk/packed params not meant for manual shell use)
- precision: decimal places for float/double display
- format: display format (hex/dec for integers, printf format for uint)
"""

import argparse
import re
from pathlib import Path

import yaml
from jinja2 import Environment, FileSystemLoader
from west import log
from west.commands import WestCommand

CONFIGEN_DESCRIPTION = """\
Generate configuration module (.c and .h files) from a YAML definition file.

The YAML file defines configuration parameters with their types, default values,
min/max constraints, and shell command help text. The generated code includes:
- Configuration structure
- Zephyr Settings handlers (load/save/export)
- Zephyr Shell commands for get/set operations
- Automatic initialization via SYS_INIT

See `configen-template.yml` in the west_commands directory for a complete
example with all supported features and detailed comments.
"""

# Path to templates directory (relative to this file)
TEMPLATES_DIR = Path(__file__).parent / "templates"

# C type mapping for all supported types
C_TYPES = {
    "bool": "bool",
    "int8": "int8_t",
    "int16": "int16_t",
    "int32": "int32_t",
    "int64": "int64_t",
    "int": "int",
    "uint8": "uint8_t",
    "uint16": "uint16_t",
    "uint32": "uint32_t",
    "uint64": "uint64_t",
    "uint": "unsigned int",
    "float": "float",
    "double": "double",
    "string": "char",
    "bytes": "uint8_t",
}

# Size in bytes for each type (for validation and display)
TYPE_SIZES = {
    "bool": 1,
    "int8": 1,
    "int16": 2,
    "int32": 4,
    "int64": 8,
    "int": 4,
    "uint8": 1,
    "uint16": 2,
    "uint32": 4,
    "uint64": 8,
    "uint": 4,
    "float": 4,
    "double": 8,
}

# Printf format specifiers
PRINTF_FORMATS = {
    "bool": "%s",
    "int8": "%d",
    "int16": "%d",
    "int32": "%d",
    "int64": "%lld",
    "int": "%d",
    "uint8": "%u",
    "uint16": "%u",
    "uint32": "%u",
    "uint64": "%llu",
    "uint": "%u",
    "float": "%.2f",
    "double": "%.2f",
    "string": "%s",
}

# Scanf/strto functions for parsing
PARSE_FUNCTIONS = {
    "int8": "strtol",
    "int16": "strtol",
    "int32": "strtol",
    "int64": "strtoll",
    "int": "strtol",
    "uint8": "strtoul",
    "uint16": "strtoul",
    "uint32": "strtoul",
    "uint64": "strtoull",
    "uint": "strtoul",
    "float": "strtof",
    "double": "strtod",
}

# Type categories for template logic
SIGNED_TYPES = {"int8", "int16", "int32", "int64", "int"}
UNSIGNED_TYPES = {"uint8", "uint16", "uint32", "uint64", "uint"}
INTEGER_TYPES = SIGNED_TYPES | UNSIGNED_TYPES
FLOAT_TYPES = {"float", "double"}
NUMERIC_TYPES = INTEGER_TYPES | FLOAT_TYPES


def filter_c_name(name):
    """Convert parameter name to C identifier (replace - with _)."""
    return name.replace("-", "_")


def filter_settings_key(name):
    """Convert parameter name to settings key (replace _ with -)."""
    return name.replace("_", "-")


def filter_c_type(param, module_name):
    """Get the C type for a parameter."""
    ptype = param.get("type")

    if ptype == "enum":
        enum_name = param.get("enum")
        return f"enum {module_name}_{filter_c_name(enum_name)}"

    return C_TYPES.get(ptype, "int32_t")


def filter_struct_field(param, module_name):
    """Generate struct field declaration for a parameter."""
    name = filter_c_name(param["name"])
    ptype = param.get("type")
    size = param.get("size")
    max_len = param.get("maxlen")
    array = param.get("array")

    c_type = filter_c_type(param, module_name)

    if ptype == "bytes" and size:
        return f"\t{c_type} {name}[{size}];"
    elif ptype == "string" and max_len:
        return f"\t{c_type} {name}[{max_len + 1}];"
    elif array:
        return f"\t{c_type} {name}[{array}];"
    else:
        return f"\t{c_type} {name};"


def filter_printf_format(param):
    """Get printf format specifier for a parameter."""
    ptype = param.get("type")
    precision = param.get("precision")
    fmt = param.get("format")

    if fmt:
        return fmt

    if ptype in FLOAT_TYPES and precision is not None:
        return f"%.{precision}f"

    return PRINTF_FORMATS.get(ptype, "%d")


def filter_parse_func(param):
    """Get the parsing function for a parameter type."""
    ptype = param.get("type")
    return PARSE_FUNCTIONS.get(ptype, "strtol")


def filter_is_signed(param):
    """Check if parameter is a signed integer type."""
    return param.get("type") in SIGNED_TYPES


def filter_is_unsigned(param):
    """Check if parameter is an unsigned integer type."""
    return param.get("type") in UNSIGNED_TYPES


def filter_is_integer(param):
    """Check if parameter is any integer type."""
    return param.get("type") in INTEGER_TYPES


def filter_is_float(param):
    """Check if parameter is a floating point type."""
    return param.get("type") in FLOAT_TYPES


def filter_is_numeric(param):
    """Check if parameter is any numeric type."""
    return param.get("type") in NUMERIC_TYPES


def filter_is_64bit(param):
    """Check if parameter is a 64-bit type."""
    return param.get("type") in {"int64", "uint64"}


def filter_default_value(param, module_name):
    """Format default value for C initialization."""
    default = param.get("default")
    ptype = param.get("type")

    if default is None:
        return None

    if ptype in FLOAT_TYPES:
        suffix = "f" if ptype == "float" else ""
        return f"{default}{suffix}"
    elif ptype == "bool":
        return "true" if default else "false"
    elif ptype == "enum":
        enum_name = param.get("enum")
        return f"{module_name.upper()}_{filter_c_name(enum_name).upper()}_{str(default).upper()}"
    elif ptype == "string":
        return f'"{default}"'
    else:
        return str(default)


def filter_min_value(param):
    """Get minimum value for range checking."""
    ptype = param.get("type")
    explicit_min = param.get("min")

    if explicit_min is not None:
        return explicit_min

    # Default minimums
    defaults = {
        "int8": "INT8_MIN",
        "int16": "INT16_MIN",
        "int32": "INT32_MIN",
        "int64": "INT64_MIN",
        "int": "INT_MIN",
        "uint8": "0",
        "uint16": "0",
        "uint32": "0",
        "uint64": "0",
        "uint": "0",
        "float": "-FLT_MAX",
        "double": "-DBL_MAX",
    }
    return defaults.get(ptype, "0")


def filter_max_value(param):
    """Get maximum value for range checking."""
    ptype = param.get("type")
    explicit_max = param.get("max")

    if explicit_max is not None:
        return explicit_max

    # Default maximums
    defaults = {
        "int8": "INT8_MAX",
        "int16": "INT16_MAX",
        "int32": "INT32_MAX",
        "int64": "INT64_MAX",
        "int": "INT_MAX",
        "uint8": "UINT8_MAX",
        "uint16": "UINT16_MAX",
        "uint32": "UINT32_MAX",
        "uint64": "UINT64_MAX",
        "uint": "UINT_MAX",
        "float": "FLT_MAX",
        "double": "DBL_MAX",
    }
    return defaults.get(ptype, "INT_MAX")


def filter_needs_cast(param):
    """Check if value needs cast for printf (for 64-bit and float types)."""
    ptype = param.get("type")
    return ptype in {"int64", "uint64", "float", "double"}


def filter_printf_cast(param):
    """Get the cast expression for printf if needed."""
    ptype = param.get("type")
    casts = {
        "int64": "(long long)",
        "uint64": "(unsigned long long)",
        "float": "(double)",
        "double": "(double)",
    }
    return casts.get(ptype, "")


# ---------------------------------------------------------------------------
# Protobuf config generation (issue #44)
#
# configen also emits the config section of the .proto (and the nanopb
# max_length options for hex-key fields) from the same YAML, so app_config.yml
# is the single source of truth for both the C struct and the wire schema.
# Existing field numbers are locked via per-parameter `proto_id`; the allocator
# only appends new ones (never renumbers) and writes them back into the YAML.
# ---------------------------------------------------------------------------

# Marker pair delimiting the generated region in the .proto / .options files.
PROTO_BEGIN = "BEGIN GENERATED CONFIG"
PROTO_END = "END GENERATED CONFIG"

# YAML type -> proto3 scalar type (mirrors the hand-written proto: every integer
# maps to uint32, byte arrays / strings become `string` with a nanopb option).
PROTO_SCALAR = {
    "bool": "bool",
    "float": "float",
    "double": "double",
    "bytes": "string",
    "string": "string",
}


def _pascal(name):
    """snake_case -> PascalCase (lrw_region -> Region after prefix strip)."""
    return "".join(w.capitalize() for w in name.split("_") if w)


def _proto_field_name(param, group):
    """Proto field name = YAML name with the group's strip_prefix removed."""
    name = param["name"]
    prefix = group.get("strip_prefix", "")
    if prefix and name.startswith(prefix):
        name = name[len(prefix):]
    return param.get("proto_name", name)


def _proto_type(param, enum_proto_name):
    ptype = param.get("type")
    if ptype == "enum":
        return enum_proto_name[param["enum"]]
    # Non-callback bytes use native protobuf `bytes` (nanopb fixed_length, see
    # build_options_lines) to avoid the 2x hex-string overhead on the wire.
    # Callback bytes (e.g. secret_key) stay `string` — their hand-written
    # callback handles the raw stream and they are off the SetParam/dump paths.
    if ptype == "bytes" and not param.get("proto_callback"):
        return "bytes"
    return PROTO_SCALAR.get(ptype, "uint32")


def _proto_groups(config):
    """Return (proto_block, {group_key: group_cfg}) or (None, None)."""
    proto = config.get("proto")
    if not proto:
        return None, None
    return proto, {g["key"]: g for g in proto["groups"]}


def allocate_proto_ids(config, rt_doc):
    """Assign a proto_id to every parameter missing one (append-only) and mirror
    the assignment back into `rt_doc` (a ruamel round-trip doc) for write-back.

    Returns the list of (name, id) newly assigned. Mutates `config` parameters
    in place so the proto model sees the final numbers.
    """
    proto, gmap = _proto_groups(config)

    # Collect the used field numbers per message namespace. The root message
    # namespace holds extra_fields(root), the root-group params and the
    # submessage container fields; each submessage namespace holds its own
    # params plus any `reserved` numbers.
    used = {g["key"]: set() for g in proto["groups"]}
    for ef in proto.get("extra_fields", []):
        used[ef.get("proto_group", "root")].add(ef["proto_id"])
    for g in proto["groups"]:
        if g["key"] != "root":
            used.setdefault("root", set()).add(g["proto_id"])
        for r in g.get("reserved", []):
            used[g["key"]].add(r)

    # Lock existing ids + detect duplicates within a namespace.
    for p in config["parameters"]:
        group = p.get("proto_group")
        if group is None:
            log.die(f"parameter '{p['name']}' is missing 'proto_group'")
        if group not in used:
            log.die(f"parameter '{p['name']}' has unknown proto_group '{group}'")
        pid = p.get("proto_id")
        if pid is not None:
            if pid in used[group]:
                log.die(f"duplicate proto_id {pid} in group '{group}' "
                        f"(parameter '{p['name']}')")
            used[group].add(pid)

    # Append-only allocation for parameters without an id, in YAML order.
    assigned = []
    for p in config["parameters"]:
        if p.get("proto_id") is None:
            group = p["proto_group"]
            nid = (max(used[group]) + 1) if used[group] else 1
            used[group].add(nid)
            p["proto_id"] = nid
            assigned.append((p["name"], nid))

    # Write the new ids back into the round-trip doc (after proto_group).
    if assigned:
        new_ids = dict(assigned)
        for p in rt_doc["parameters"]:
            if p["name"] in new_ids and "proto_id" not in p:
                idx = list(p.keys()).index("proto_group") + 1
                p.insert(idx, "proto_id", new_ids[p["name"]])

    return assigned


def _parse_proto_field_ids(text):
    """Extract {field_name: number} from `optional <type> <name> = <n>;` lines in
    the existing generated region (used by the no-renumber guard)."""
    ids = {}
    region = text
    if PROTO_BEGIN in text and PROTO_END in text:
        region = text.split(PROTO_BEGIN, 1)[1].split(PROTO_END, 1)[0]
    for m in re.finditer(r"optional\s+\S+\s+(\w+)\s*=\s*(\d+)\s*;", region):
        ids[m.group(1)] = int(m.group(2))
    return ids


def guard_no_renumber(config, proto_path):
    """Fail the run if a parameter's locked proto field number differs from the
    one already in the target .proto (someone edited a YAML proto_id)."""
    if not proto_path.exists():
        return
    old_ids = _parse_proto_field_ids(proto_path.read_text())
    if not old_ids:
        return
    _, gmap = _proto_groups(config)
    for p in config["parameters"]:
        fname = _proto_field_name(p, gmap[p["proto_group"]])
        if fname in old_ids and old_ids[fname] != p["proto_id"]:
            log.die(f"proto_id for '{fname}' changed {old_ids[fname]} -> "
                    f"{p['proto_id']} — renumbering is forbidden (NFC tags / "
                    f"downlinks already deployed)")


def build_proto_model(config):
    """Build the jinja2 context for config.proto.j2 from the YAML config."""
    proto, gmap = _proto_groups(config)
    enums = config.get("enums", {})

    by_group = {g["key"]: [] for g in proto["groups"]}
    for p in config["parameters"]:
        by_group[p["proto_group"]].append(p)

    # Map each YAML enum to its proto type name, derived in the group that uses
    # it (strip that group's prefix, PascalCase the remainder).
    enum_proto_name = {}
    for gkey, plist in by_group.items():
        for p in plist:
            if p.get("type") == "enum" and p["enum"] not in enum_proto_name:
                base = p["enum"]
                prefix = gmap[gkey].get("strip_prefix", "")
                if prefix and base.startswith(prefix):
                    base = base[len(prefix):]
                enum_proto_name[p["enum"]] = _pascal(base)

    def field(name, ptype, fid):
        return {"name": name, "ptype": ptype, "id": fid}

    # Root message: extra_fields(root) + root-group params + submessage
    # containers, all in the root namespace, ordered by field number.
    root_fields = []
    for ef in proto.get("extra_fields", []):
        if ef.get("proto_group", "root") == "root":
            root_fields.append(field(ef["name"], ef["type"], ef["proto_id"]))
    for p in by_group.get("root", []):
        root_fields.append(field(_proto_field_name(p, gmap["root"]),
                                  _proto_type(p, enum_proto_name), p["proto_id"]))
    for g in proto["groups"]:
        if g["key"] != "root":
            root_fields.append(field(g["field"], g["message"], g["proto_id"]))
    root_fields.sort(key=lambda f: f["id"])

    submessages = []
    for g in proto["groups"]:
        if g["key"] == "root":
            continue
        plist = sorted(by_group.get(g["key"], []), key=lambda p: p["proto_id"])
        genums, seen = [], set()
        for p in plist:
            if p.get("type") == "enum" and p["enum"] not in seen:
                seen.add(p["enum"])
                genums.append({
                    "name": enum_proto_name[p["enum"]],
                    "members": [{"name": v["name"], "value": v["value"]}
                                for v in enums[p["enum"]]],
                })
        fields = [field(_proto_field_name(p, g), _proto_type(p, enum_proto_name),
                        p["proto_id"]) for p in plist]
        submessages.append({
            "name": g["message"],
            "enums": genums,
            "reserved": list(g.get("reserved", [])),
            "fields": fields,
        })

    return {"message": proto["message"], "root_fields": root_fields,
            "submessages": submessages}


def build_ingest_model(config):
    """jinja context for config_ingest.c.j2: the non-root submessage groups with
    everything the generated apply_/fill_ functions need (proto field name, C
    field name, per-type handling kind, range/enum metadata)."""
    proto, gmap = _proto_groups(config)
    module = config["module"]["name"]
    enums = config.get("enums", {})

    by_group = {g["key"]: [] for g in proto["groups"]}
    for p in config["parameters"]:
        by_group[p["proto_group"]].append(p)

    enum_proto_name = {}
    for gkey, plist in by_group.items():
        for p in plist:
            if p.get("type") == "enum" and p["enum"] not in enum_proto_name:
                base = p["enum"]
                prefix = gmap[gkey].get("strip_prefix", "")
                if prefix and base.startswith(prefix):
                    base = base[len(prefix):]
                enum_proto_name[p["enum"]] = _pascal(base)

    groups = []
    for g in proto["groups"]:
        if g["key"] == "root":
            continue
        c_message = f"{proto['message']}_{g['message']}"
        params = []
        for p in sorted(by_group.get(g["key"], []), key=lambda x: x["proto_id"]):
            t = p.get("type")
            e = {
                "c_name": filter_c_name(p["name"]),
                "proto_name": _proto_field_name(p, g),
                "tag": p["proto_id"],
                "dump": p.get("dump", True),
                "dump_nfc_only": bool(p.get("dump_nfc_only")),
                "callback": bool(p.get("proto_callback")),
            }
            if t == "bool":
                e["kind"] = "bool"
            elif t == "enum":
                e["kind"] = "enum"
                e["enum_max"] = max(v["value"] for v in enums[p["enum"]])
                e["c_enum_type"] = filter_c_type(p, module)
                e["proto_enum_type"] = f"{c_message}_{enum_proto_name[p['enum']]}"
            elif t == "bytes":
                e["kind"] = "bytes"
            elif t in FLOAT_TYPES:
                e["kind"] = "float"
                e["min"] = filter_min_value(p)
                e["max"] = filter_max_value(p)
            elif t in NUMERIC_TYPES:
                if p.get("min") is not None or p.get("max") is not None:
                    e["kind"] = "int_ranged"
                    e["min"] = filter_min_value(p)
                    e["max"] = filter_max_value(p)
                    e["zero_allowed"] = bool(p.get("extras", {}).get("zero_allowed"))
                else:
                    e["kind"] = "plain"
            else:
                e["kind"] = "plain"
            params.append(e)
        groups.append({
            "key": g["key"],
            "c_message": c_message,
            "apply_fn": f"app_config_apply_{g['key']}",
            "fill_fn": f"app_config_fill_{g['key']}",
            "params": params,
        })
    return {"message": proto["message"], "ingest_groups": groups}


def build_options_lines(config):
    """nanopb options for bytes fields: native `bytes` with fixed_length (a plain
    `pb_byte_t[size]`, no length word) — half the wire size of the old hex string
    and the C struct stays `uint8_t[size]`. Fields flagged `proto_callback: true`
    are left as nanopb callbacks (no option emitted)."""
    proto, gmap = _proto_groups(config)
    msg = proto["message"]
    lines = []
    for p in config["parameters"]:
        if p.get("type") != "bytes" or p.get("proto_callback"):
            continue
        g = gmap[p["proto_group"]]
        fname = _proto_field_name(p, g)
        path = (f"{msg}.{fname}" if g["key"] == "root"
                else f"{msg}.{g['message']}.{fname}")
        lines.append(f"{path} max_size:{p['size']} fixed_length:true")
    return lines


def rewrite_marked_text(text, body, comment, begin_label, end_label, where=""):
    """Replace the text between the BEGIN/END markers in `text` with `body`."""
    begin = f"{comment} {begin_label}"
    end = f"{comment} {end_label}"
    if begin not in text or end not in text:
        log.die(f"{where or 'input'} is missing the generated-region markers "
                f"({begin} / {end})")
    head, rest = text.split(begin, 1)
    _, tail = rest.split(end, 1)
    return f"{head}{begin}\n{body}\n{end}{tail}"


def rewrite_marked_region(path, body, comment,
                          begin_label=PROTO_BEGIN, end_label=PROTO_END):
    """Replace the text between the BEGIN/END markers in `path` with `body`,
    keeping everything outside (hand-written messages / options). `begin_label`
    / `end_label` select which marker pair (a file may hold several, e.g. the
    config region and the Command-oneof region in the .proto)."""
    if not path.exists():
        log.die(f"target {path} not found — seed it with the "
                f"'{comment} {begin_label}' / '{comment} {end_label}' markers first")
    return rewrite_marked_text(path.read_text(), body, comment,
                               begin_label, end_label, str(path))


# Marker labels for the command-codegen regions (each lives in its own file:
# the Command oneof in the .proto, the dispatch switch in app_cmd.c, the
# name<->tag maps in ttn.js). All use line comments so one rewrite helper fits.
COMMANDS_BEGIN = "BEGIN GENERATED COMMANDS"
COMMANDS_END = "END GENERATED COMMANDS"
DISPATCH_BEGIN = "BEGIN GENERATED DISPATCH"
DISPATCH_END = "END GENERATED DISPATCH"
DUMP_FIELDS_BEGIN = "BEGIN GENERATED DUMP_FIELDS"
DUMP_FIELDS_END = "END GENERATED DUMP_FIELDS"

# Response-body kinds whose dispatch emits no immediate Response (the command's
# effect — telemetry, history frames, a deferred Info — is the answer).
COMMAND_RESPONSE_NONE = {"none", "info_deferred"}


def guard_no_renumber_commands(model, proto_path):
    """Fail the run if a command's proto_id differs from the one already in the
    Command oneof region of the target .proto (a deployed downlink / NFC tag
    encodes the tag, so renumbering silently breaks the field protocol)."""
    if not proto_path.exists():
        return
    text = proto_path.read_text()
    begin = f"// {COMMANDS_BEGIN}"
    end = f"// {COMMANDS_END}"
    if begin not in text or end not in text:
        return
    region = text.split(begin, 1)[1].split(end, 1)[0]
    old = {}
    for m in re.finditer(r"^\s*\w+\s+(\w+)\s*=\s*(\d+)\s*;", region, re.MULTILINE):
        old[m.group(1)] = int(m.group(2))
    for c in model["commands"]:
        if c["name"] in old and old[c["name"]] != c["proto_id"]:
            log.die(f"command proto_id for '{c['name']}' changed "
                    f"{old[c['name']]} -> {c['proto_id']} — renumbering is "
                    f"forbidden (downlinks / NFC tags already deployed)")


def build_commands_model(config):
    """jinja context for the command codegen (proto Command oneof, decoder
    _CMD_NAMES/_CMD_TAGS, app_cmd_dispatch switch) from the YAML `commands:`
    section. Returns None when the YAML has no command list."""
    commands = config.get("commands")
    if not commands:
        return None

    msg = commands["message"]
    seen_ids, seen_names = {}, set()
    cmds = []
    for c in commands["list"]:
        name = c["name"]
        pid = c["proto_id"]
        if name in seen_names:
            log.die(f"duplicate command name '{name}'")
        if pid in seen_ids:
            log.die(f"duplicate command proto_id {pid} "
                    f"('{name}' vs '{seen_ids[pid]}')")
        seen_names.add(name)
        seen_ids[pid] = name

        kind = c["kind"]
        if kind not in ("handler", "action"):
            log.die(f"command '{name}' has invalid kind '{kind}' "
                    f"(expected handler|action)")
        if kind == "action" and not c.get("action"):
            log.die(f"action command '{name}' must set 'action'")

        transports = c.get("transports")
        cmds.append({
            "name": name,
            "proto_id": pid,
            "body": c["body"],
            "kind": kind,
            "action": c.get("action"),
            "response": c.get("response", "ack"),
            "transports": transports,
            "lrw_only": transports == ["lrw"],
            "emits_response": c.get("response", "ack") not in COMMAND_RESPONSE_NONE,
            "tag": f"{msg}_{name}_tag",
            "handler": f"app_cmd_handle_{name}",
        })

    by_id = sorted(cmds, key=lambda c: c["proto_id"])
    return {
        "message": msg,
        "response_message": commands["response"],
        "reserved": list(commands.get("reserved", [])),
        "commands": cmds,        # YAML order (decoder/dispatch)
        "commands_by_id": by_id, # proto_id order (proto oneof)
        # Column widths so the generated .proto oneof aligns like the rest.
        "type_width": max(len(c["body"]) for c in cmds),
        "name_width": max(len(c["name"]) for c in cmds),
    }


# get_config DUMP_FIELDS[] codegen ------------------------------------------
# Section order MUST match the DUMP_SECTION_* enum in app_cmd.c.
DUMP_SECTIONS = ["lorawan", "application", "sensors", "alarms"]


def _varint_bytes(value):
    """Encoded length of `value` as an unsigned protobuf varint (>= 1 byte)."""
    v = int(value)
    n = 1
    while v >= 0x80:
        v >>= 7
        n += 1
    return n


def _dump_field_size(p):
    """Conservative upper bound on the encoded bytes of one config field inside a
    ConfigDump: proto tag + value, matching the on-wire encoding configen emits.
    Tags for proto_id >= 16 take two bytes; integers up to their declared max;
    bytes are emitted as a hex string (2 chars/byte + a length byte)."""
    tag = 1 if p["proto_id"] <= 15 else 2
    t = p.get("type")
    if t == "bytes":
        n = 2 * int(p["size"])
        return tag + _varint_bytes(n) + n
    if t in ("bool", "enum"):
        return tag + 1
    # Integers map to a varint. All current config ints are non-negative, so the
    # declared max bounds the width; fall back to the uint32 worst case (5 B)
    # when unbounded or potentially negative.
    mx = p.get("max")
    mn = p.get("min")
    if mx is not None and (mn is None or mn >= 0):
        return tag + _varint_bytes(int(mx))
    return tag + 5


def build_dump_fields_model(config):
    """Rows for the get_config DUMP_FIELDS[] table: every dumpable parameter
    (proto_group in a ConfigDump section and not `dump: false`), grouped by
    section in DUMP_SECTION order, YAML declaration order within a section.

    A `dump_nfc_only: true` field is included with nfc_only=1; the handler only
    selects it when the transport is NFC, so it never enters a LoRaWAN response
    (e.g. the LoRaWAN crypto keys — readable over the encrypted NFC channel only).
    A plain `dump: false` field stays excluded from every transport."""
    rows = []
    for section in DUMP_SECTIONS:
        macro = "DUMP_SECTION_" + section.upper()
        for p in config["parameters"]:
            if p.get("proto_group") != section or "proto_id" not in p:
                continue
            nfc_only = bool(p.get("dump_nfc_only"))
            if p.get("dump") is False and not nfc_only:
                continue
            rows.append({"section": macro, "tag": p["proto_id"],
                         "size": _dump_field_size(p), "nfc_only": nfc_only})
    return {"dump_fields": rows}


class Configen(WestCommand):
    def __init__(self):
        super().__init__(
            "configen",
            "generate configuration module from YAML",
            CONFIGEN_DESCRIPTION,
            accepts_unknown_args=False,
            requires_workspace=False,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            formatter_class=argparse.RawDescriptionHelpFormatter,
            description=self.description,
        )

        parser.add_argument(
            "yaml_file", type=Path, help="path to the YAML configuration file"
        )
        parser.add_argument(
            "-o",
            "--output-dir",
            type=Path,
            default=None,
            help="output directory for generated files (default: same as YAML file)",
        )
        parser.add_argument(
            "-t",
            "--templates-dir",
            type=Path,
            default=None,
            help="custom templates directory (default: built-in templates)",
        )
        parser.add_argument(
            "--proto",
            type=Path,
            default=None,
            help="path to the .proto whose generated region is rewritten "
            "(default: <output-dir>/<module>.proto). Only used when the YAML "
            "has a 'proto:' block.",
        )
        parser.add_argument(
            "--options",
            type=Path,
            default=None,
            help="path to the nanopb .options.in whose generated region is "
            "rewritten (default: <output-dir>/<module>.options.in)",
        )
        parser.add_argument(
            "--no-proto",
            action="store_true",
            help="skip proto/options generation even if the YAML has a "
            "'proto:' block",
        )
        parser.add_argument(
            "--decoder",
            type=Path,
            default=None,
            help="path to the JS decoder whose _CMD_NAMES/_CMD_TAGS region is "
            "rewritten from the YAML 'commands:' list "
            "(default: <output-dir>/../decoder/ttn.js)",
        )
        parser.add_argument(
            "--app-cmd",
            type=Path,
            default=None,
            help="path to the C command file whose app_cmd_dispatch() region is "
            "rewritten from the YAML 'commands:' list "
            "(default: <output-dir>/app_cmd.c)",
        )
        parser.add_argument(
            "--dry-run",
            action="store_true",
            help="print what would be generated without writing files",
        )

        return parser

    def do_run(self, args, unknown_args):
        yaml_path = args.yaml_file.resolve()

        if not yaml_path.exists():
            log.die(f"YAML file not found: {yaml_path}")

        # Load YAML configuration
        with open(yaml_path, "r") as f:
            try:
                config = yaml.safe_load(f)
            except yaml.YAMLError as e:
                log.die(f"Failed to parse YAML file: {e}")

        if config is None:
            log.die("YAML file is empty")

        # Validate required fields
        version = config.get("version")
        if version is None:
            log.die("YAML file must have a 'version' field")
        if version != 1:
            log.die(f"Unsupported YAML version: {version} (expected: 1)")

        module = config.get("module")
        if module is None:
            log.die("YAML file must have a 'module' field")

        if module.get("name") is None:
            log.die("Module must have a 'name' field")

        parameters = config.get("parameters", [])
        enums = config.get("enums", {})

        # Validate parameters
        for param in parameters:
            self._validate_param(param)

        # Setup Jinja2 environment
        templates_dir = args.templates_dir or TEMPLATES_DIR
        if not templates_dir.exists():
            log.die(f"Templates directory not found: {templates_dir}")

        env = Environment(
            loader=FileSystemLoader(templates_dir),
            trim_blocks=True,
            lstrip_blocks=True,
            keep_trailing_newline=True,
        )

        # Register filters
        env.filters["c_name"] = filter_c_name
        env.filters["settings_key"] = filter_settings_key
        env.filters["c_type"] = filter_c_type
        env.filters["struct_field"] = filter_struct_field
        env.filters["printf_format"] = filter_printf_format
        env.filters["parse_func"] = filter_parse_func
        env.filters["is_signed"] = filter_is_signed
        env.filters["is_unsigned"] = filter_is_unsigned
        env.filters["is_integer"] = filter_is_integer
        env.filters["is_float"] = filter_is_float
        env.filters["is_numeric"] = filter_is_numeric
        env.filters["is_64bit"] = filter_is_64bit
        env.filters["default_value"] = filter_default_value
        env.filters["min_value"] = filter_min_value
        env.filters["max_value"] = filter_max_value
        env.filters["needs_cast"] = filter_needs_cast
        env.filters["printf_cast"] = filter_printf_cast

        # Prepare template context
        context = {
            "module": module,
            "parameters": parameters,
            "enums": enums,
            # Type categories for template conditionals
            "SIGNED_TYPES": SIGNED_TYPES,
            "UNSIGNED_TYPES": UNSIGNED_TYPES,
            "INTEGER_TYPES": INTEGER_TYPES,
            "FLOAT_TYPES": FLOAT_TYPES,
            "NUMERIC_TYPES": NUMERIC_TYPES,
        }

        # Render templates
        try:
            header_template = env.get_template("config.h.j2")
            source_template = env.get_template("config.c.j2")
        except Exception as e:
            log.die(f"Failed to load templates: {e}")

        header_content = header_template.render(**context)
        source_content = source_template.render(**context)

        # Determine output paths
        output_dir = args.output_dir or yaml_path.parent
        output_dir = output_dir.resolve()
        module_name = module["name"]

        header_path = output_dir / f"{module_name}.h"
        source_path = output_dir / f"{module_name}.c"

        if args.dry_run:
            log.inf(f"Would generate: {header_path}")
            log.inf("--- Header content ---")
            print(header_content)
            log.inf(f"\nWould generate: {source_path}")
            log.inf("--- Source content ---")
            print(source_content)
        else:
            output_dir.mkdir(parents=True, exist_ok=True)

            with open(header_path, "w") as f:
                f.write(header_content)
            log.inf(f"Generated: {header_path}")

            with open(source_path, "w") as f:
                f.write(source_content)
            log.inf(f"Generated: {source_path}")

            self._clang_format([header_path, source_path])

        # Config ingest (apply/fill per proto submessage) — generated so the
        # SetParam/NFC/GetConfig mapping can't drift from the YAML (#44/#91).
        ingest_path = output_dir / f"{module_name}_ingest.c"
        if config.get("proto"):
            try:
                ingest_template = env.get_template("config_ingest.c.j2")
            except Exception as e:
                log.die(f"Failed to load config_ingest.c.j2: {e}")
            ingest_content = ingest_template.render(**build_ingest_model(config),
                                                    module=module)
            if args.dry_run:
                log.inf(f"Would generate: {ingest_path}")
                print(ingest_content)
            else:
                with open(ingest_path, "w") as f:
                    f.write(ingest_content)
                log.inf(f"Generated: {ingest_path}")
                self._clang_format([ingest_path])

        # Proto + nanopb options generation (issue #44).
        if config.get("proto") and not args.no_proto:
            proto_path = args.proto or (output_dir / f"{module_name}.proto")
            options_path = args.options or (output_dir / f"{module_name}.options.in")
            self._generate_proto(env, config, yaml_path, proto_path.resolve(),
                                 options_path.resolve(), args.dry_run)

        # Command protocol codegen (decoder maps, dispatch switch, Command oneof)
        # from the YAML `commands:` section — keeps the wire id / routing /
        # availability of every command in one place (configen-commands design).
        commands_model = build_commands_model(config)
        if commands_model:
            commands_model.update(build_dump_fields_model(config))
            self._generate_commands(env, commands_model, output_dir, args)

    def _generate_proto(self, env, config, yaml_path, proto_path, options_path,
                        dry_run):
        """Generate the .proto config region + nanopb options from the YAML,
        with an append-only proto_id allocator that writes new ids back."""
        from ruamel.yaml import YAML

        ryaml = YAML()
        ryaml.preserve_quotes = True
        ryaml.width = 4096
        ryaml.indent(mapping=2, sequence=4, offset=2)
        with open(yaml_path) as f:
            rt_doc = ryaml.load(f)

        # Allocate ids for new params, guard against renumbering existing ones.
        assigned = allocate_proto_ids(config, rt_doc)
        guard_no_renumber(config, proto_path)

        model = build_proto_model(config)
        body = env.get_template("config.proto.j2").render(proto=model).rstrip("\n")
        proto_text = rewrite_marked_region(proto_path, body, "//")

        # Command oneof: a second generated region in the same .proto (the body
        # sub-messages + Response stay hand-written). Rewritten in-memory on top
        # of the config region, when the YAML has a commands list and the file
        # carries the markers.
        commands_model = build_commands_model(config)
        if commands_model and f"// {COMMANDS_BEGIN}" in proto_text:
            guard_no_renumber_commands(commands_model, proto_path)
            cmd_body = env.get_template("commands_proto.j2").render(
                **commands_model).rstrip("\n")
            proto_text = rewrite_marked_text(proto_text, cmd_body, "//",
                                             COMMANDS_BEGIN, COMMANDS_END,
                                             str(proto_path))

        options_body = "\n".join(build_options_lines(config))
        options_text = rewrite_marked_region(options_path, options_body, "#")

        if dry_run:
            for name, fid in assigned:
                log.inf(f"Would assign proto_id {fid} to '{name}'")
            log.inf(f"Would generate: {proto_path}")
            log.inf("--- Proto generated region ---")
            print(body)
            log.inf(f"\nWould generate: {options_path}")
            print(options_body)
            return

        for name, fid in assigned:
            log.inf(f"Assigned proto_id {fid} to new parameter '{name}'")
        if assigned:
            with open(yaml_path, "w") as f:
                ryaml.dump(rt_doc, f)
            log.inf(f"Wrote new proto_id(s) back into: {yaml_path}")

        proto_path.write_text(proto_text)
        log.inf(f"Generated: {proto_path}")
        options_path.write_text(options_text)
        log.inf(f"Generated: {options_path}")

    def _generate_commands(self, env, model, output_dir, args):
        """Rewrite the command-codegen regions from the YAML `commands:` list:
        the decoder _CMD_NAMES map (ttn.js) and the app_cmd_dispatch() switch
        (app_cmd.c). The proto Command oneof is rewritten by _generate_proto."""
        # Decoder: _CMD_NAMES / _CMD_TAGS region in ttn.js.
        decoder_path = args.decoder or (output_dir.parent / "decoder" / "ttn.js")
        decoder_path = decoder_path.resolve()
        if decoder_path.exists():
            body = env.get_template("commands_decoder.js.j2").render(**model)
            body = body.rstrip("\n")
            text = rewrite_marked_region(decoder_path, body, "//",
                                         COMMANDS_BEGIN, COMMANDS_END)
            if args.dry_run:
                log.inf(f"Would generate decoder region in: {decoder_path}")
                print(body)
            else:
                decoder_path.write_text(text)
                log.inf(f"Generated decoder command region: {decoder_path}")
        else:
            log.wrn(f"decoder not found ({decoder_path}); "
                    "skipping _CMD_NAMES region")

        # Firmware: app_cmd_dispatch() switch region in app_cmd.c. Only rewritten
        # once the file carries the markers (seeded when the dispatch codegen
        # lands); skipped gracefully otherwise so the decoder region can ship
        # ahead of it.
        app_cmd_path = args.app_cmd or (output_dir / "app_cmd.c")
        app_cmd_path = app_cmd_path.resolve()
        if app_cmd_path.exists():
            text = app_cmd_path.read_text()
            changed = False

            if f"// {DISPATCH_BEGIN}" in text:
                body = env.get_template("commands_dispatch.c.j2").render(**model).rstrip("\n")
                text = rewrite_marked_text(text, body, "//", DISPATCH_BEGIN, DISPATCH_END,
                                           str(app_cmd_path))
                changed = True
                if args.dry_run:
                    log.inf(f"Would generate dispatch region in: {app_cmd_path}")
                    print(body)

            # get_config DUMP_FIELDS[] paging table (#112).
            if "dump_fields" in model and f"// {DUMP_FIELDS_BEGIN}" in text:
                body = env.get_template("dump_fields.c.j2").render(**model).rstrip("\n")
                text = rewrite_marked_text(text, body, "//", DUMP_FIELDS_BEGIN, DUMP_FIELDS_END,
                                           str(app_cmd_path))
                changed = True
                if args.dry_run:
                    log.inf(f"Would generate DUMP_FIELDS region in: {app_cmd_path}")
                    print(body)

            if changed and not args.dry_run:
                app_cmd_path.write_text(text)
                log.inf(f"Generated app_cmd regions: {app_cmd_path}")
                self._clang_format([app_cmd_path])

    def _clang_format(self, paths):
        """Run clang-format on generated files if available."""
        import shutil
        import subprocess

        clang_format = shutil.which("clang-format")
        if not clang_format:
            log.wrn("clang-format not found in PATH; skipping formatting. "
                    "Generated output will use template wrap style.")
            return

        try:
            subprocess.run([clang_format, "-i", *[str(p) for p in paths]],
                           check=True)
            log.inf(f"Formatted with clang-format: {', '.join(p.name for p in paths)}")
        except subprocess.CalledProcessError as e:
            log.wrn(f"clang-format failed: {e}; generated files left as-is")

    def _validate_param(self, param):
        """Validate a parameter definition."""
        name = param.get("name")
        if not name:
            log.die("Parameter must have a 'name' field")

        ptype = param.get("type")
        if not ptype:
            log.die(f"Parameter '{name}' must have a 'type' field")

        valid_types = set(C_TYPES.keys()) | {"enum"}
        if ptype not in valid_types:
            log.die(f"Parameter '{name}' has invalid type '{ptype}'. Valid types: {sorted(valid_types)}")

        if ptype == "bytes" and not param.get("size"):
            log.die(f"Parameter '{name}' of type 'bytes' must have a 'size' field")

        if ptype == "string" and not param.get("maxlen"):
            log.die(f"Parameter '{name}' of type 'string' must have a 'maxlen' field")

        if ptype == "enum" and not param.get("enum"):
            log.die(f"Parameter '{name}' of type 'enum' must have an 'enum' field")
