"""pytest setup for the configen test suite.

Puts the west_commands directory (where configen.py lives) on sys.path so the
tests can `import configen` directly and exercise its functions.
"""
import sys
from pathlib import Path

WEST_COMMANDS_DIR = Path(__file__).resolve().parent.parent
if str(WEST_COMMANDS_DIR) not in sys.path:
    sys.path.insert(0, str(WEST_COMMANDS_DIR))
