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
- precision: decimal places for float/double display
- format: display format (hex/dec for integers, printf format for uint)
"""

import argparse
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
