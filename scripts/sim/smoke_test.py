#!/usr/bin/env python3
#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

"""End-to-end smoke test of the STICKER simulator (doc/simulator.md), as run
by CI: one simulated STICKER (zephyr.exe) and the air peer (sim_air.py).

  1. LoRaWAN: provision over the shell, OTAA join, decoded telemetry at the LNS
  2. NFC: plaintext get_basic_info over the emulated ST25DV (sim_nfc.py)
  3. P2P: switch to TOWER over LoRa, join the hub, decoded telemetry
  4. Sensors: the emulated barometer, accelerometer and 1-Wire bus answer

Exit 0 when every step passed; on failure the tails of both logs are printed.
"""

import argparse
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
APPKEY = "000102030405060708090a0b0c0d0e0f"
DEVEUI = "5876070000000001"


class Proc:
    """A child process whose stdout lines are kept and can be waited for."""

    def __init__(self, name, argv, log_dir):
        self.name = name
        self.lines = []
        self.new = queue.Queue()
        self.log = open(log_dir / f"{name}.log", "w")
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.log.write(line)
            self.log.flush()
            self.lines.append(line.rstrip("\n"))
            self.new.put(None)

    def mark(self):
        return len(self.lines)

    def expect(self, pattern, timeout, since=0):
        """First line matching pattern at or after index since."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        i = since
        while True:
            while i < len(self.lines):
                m = rx.search(self.lines[i])
                if m:
                    return m
                i += 1
            left = deadline - time.monotonic()
            if left <= 0 or self.proc.poll() is not None:
                raise TimeoutError(f"{self.name}: no line matching /{pattern}/ in {timeout} s")
            try:
                self.new.get(timeout=min(left, 1.0))
            except queue.Empty:
                pass

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        time.sleep(0.3)  # the shell RX ring is small: pace the input

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.log.close()

    def tail(self, n=40):
        return "\n".join(self.lines[-n:])


def step(name, fn):
    t0 = time.monotonic()
    fn()
    print(f"PASS {name} ({time.monotonic() - t0:.1f} s)", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default="build/zephyr/zephyr.exe", help="simulator executable")
    ap.add_argument("--log-dir", help="keep the logs here (default: a temporary directory)")
    args = ap.parse_args()

    log_dir = Path(args.log_dir or tempfile.mkdtemp(prefix="sim_smoke_"))
    log_dir.mkdir(parents=True, exist_ok=True)
    flash = log_dir / "flash.bin"
    flash.unlink(missing_ok=True)

    air = Proc("air", [sys.executable, "-u", str(HERE / "sim_air.py"), "--appkey", APPKEY],
               log_dir)
    sim = Proc("sim", [os.path.abspath(args.exe), f"--flash={flash}"], log_dir)

    def boot():
        sim.expect(r"main: Alive", 30)

    def lorawan():
        at = air.mark()
        for cmd in (f"config radio-deveui {DEVEUI}", f"config radio-appkey {APPKEY}",
                    "config radio-mode lorawan", "config cap-barometer true",
                    "config cap-accelerometer true", "config cap-w1-sensors true",
                    "config accel-motion-sensitivity high", "config interval-sample 5",
                    "settings save"):
            sim.send(cmd)
        air.expect(rf"{DEVEUI} FCnt \d+ FPort 2 ", 120, at)
        air.expect(r'decoded \{"voltage"', 10, at)

    def nfc():
        out = subprocess.run([sys.executable, str(HERE / "sim_nfc.py"), "basic"],
                             capture_output=True, text=True, timeout=60)
        if out.returncode != 0 or "basic_info" not in out.stdout:
            raise RuntimeError(f"sim_nfc.py basic failed:\n{out.stdout}{out.stderr}")

    def sensors():
        at = sim.mark()
        sim.send("w1 enroll")
        sim.expect(r"Slot 1 auto-enrolled dallas", 30, at)
        sim.send("sim baro 950 18")
        sim.send("sim accel shake")
        sim.expect(r"Accelerometer motion detected", 10, at)
        sim.expect(r"Pressure: 950\.00 hPa", 30, at)

    def p2p():
        at = air.mark()
        sim.send("config radio-mode p2p")
        sim.send("settings save")
        air.expect(rf"p2p {DEVEUI}: JoinRequest", 60, at)
        air.expect(rf"p2p {DEVEUI}: up \d+ C? ?port 2 ", 60, at)

    try:
        step("boot", boot)
        step("lorawan join + telemetry", lorawan)
        step("nfc basic_info", nfc)
        step("sensors (1-Wire, barometer, accelerometer)", sensors)
        step("p2p join + telemetry", p2p)
    except (TimeoutError, RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"FAIL {e}\n--- sim ---\n{sim.tail()}\n--- air ---\n{air.tail()}", flush=True)
        return 1
    finally:
        sim.stop()
        air.stop()
        print(f"logs: {log_dir}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
