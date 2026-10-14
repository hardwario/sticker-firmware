#!/usr/bin/env python3
#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#
"""The v2.0.0 HIL plan (H1-H7, `doc/plan/509 - v2.0.0 release review fixes.md`)
run on the simulator: pulse counter, RATE alarms, stillness rules, the 11 B tier,
settings save, the v1.4 stale-key upgrade, buzzer and PIR modes. Each step
starts its own STICKER with a fresh flash file; H2 and H5 add `sim_air.py`.

H6 needs the boot-time log of app_config_init, which the shell log backend
misses: build with -DCONFIG_LOG_MODE_IMMEDIATE=y -DCONFIG_LOG_BACKEND_UART=y.
The whole run takes about 20 minutes.

    python scripts/sim/hil_v200.py --exe app/build-sim/zephyr/zephyr.exe h1 h6
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from smoke_test import APPKEY, DEVEUI, Proc

sim_dir = Path(__file__).resolve().parent
_ap = argparse.ArgumentParser(description=__doc__,
                              formatter_class=argparse.RawDescriptionHelpFormatter)
_ap.add_argument("--exe", default="build/zephyr/zephyr.exe", help="simulator executable")
_ap.add_argument("--log-dir", help="keep the logs here (default: a temporary directory)")
_ap.add_argument("tests", nargs="*", help="steps to run: h1 .. h7 (default: all)")
_args = _ap.parse_args()
exe = os.path.abspath(_args.exe)
logd = Path(_args.log_dir or tempfile.mkdtemp(prefix="sim_hil_"))
logd.mkdir(parents=True, exist_ok=True)
only = set(_args.tests)

DECODER = (sim_dir.parent.parent / "app/decoder/ttn.js").resolve()
BOOT = r"RF_WRITE_EN configured"
RESULTS = []


def result(tid, ok, note):
    RESULTS.append((tid, "PASS" if ok else "FAIL", note))
    print(f"{'PASS' if ok else 'FAIL'} {tid}: {note}", flush=True)


def start(name, air=False, extra=(), fresh=True):
    flash = logd / f"{name}.bin"
    if fresh:
        flash.unlink(missing_ok=True)
    argv = [exe, f"--flash={flash}", "--nfc_off", *extra]
    if not air:
        argv.append("--air_off")
    s = Proc(name, argv, logd)
    s.expect(BOOT, 30)
    time.sleep(1.5)
    return s


def save(s, cmds, reboot=True):
    m = s.mark()
    for c in cmds:
        s.send(c)
    s.send("settings save")
    if reboot:
        s.expect(BOOT, 30, m + 1)
        time.sleep(1.5)


def ask(s, cmd, pat, timeout=5):
    m = s.mark()
    s.send(cmd)
    return s.expect(pat, timeout, m)


def count_a(s):
    return int(ask(s, "ats sensors sample", r"input-a:\s+count=(\d+)").group(1))


def alarm_active(s, slot=0):
    m = s.mark()
    s.send("alarm list")
    time.sleep(0.4)
    act = [int(x) for l in s.lines[m:] for x in re.findall(r"active=(\d)", l)]
    return act


# --- H1 pulse counter (D7) ------------------------------------------------
def h1():
    base = ["config cap-gpio-mode inputs", "config cap-gpio-input-a pulse",
            "config input-a-counter true"]
    # idle high (line pulled up, contact to GND), 2 Hz with 3 ms bounce
    s = start("h1_hi")
    save(s, base)
    c0 = count_a(s)
    s.send("sim pulse gp_a 100 250 3")
    time.sleep(100 * 0.5 + 3)
    n = count_a(s) - c0
    result("H1.2-3", n == 100, f"idle-high, 100 pulses 2 Hz + bounce -> {n}")
    # 10 Hz, 30 ms pulses (S0)
    c0 = count_a(s)
    s.send("sim pulse gp_a 50 30")
    time.sleep(50 * 0.06 + 2)
    n = count_a(s) - c0
    result("H1.5", n == 50, f"30 ms pulses x50 -> {n}")
    # regression of the 20 ms settle bug: 15 ms pulses must count too
    c0 = count_a(s)
    s.send("sim pulse gp_a 50 15")
    time.sleep(50 * 0.03 + 2)
    n = count_a(s) - c0
    result("H1.5b", n == 50, f"15 ms pulses x50 -> {n}")
    s.stop()

    # idle low (relay sourcing the line)
    s = start("h1_lo", extra=["--pin_low=gp_a"])
    save(s, base)
    c0 = count_a(s)
    s.send("sim pulse gp_a 100 250 3")
    time.sleep(53)
    n = count_a(s) - c0
    result("H1.4", n == 100, f"idle-low, 100 pulses -> {n}")
    s.stop()

    # boot with the line held active (idle high, active = low), then release
    s = start("h1_boot")
    save(s, base, reboot=False)
    time.sleep(1)
    s.stop()
    s = start("h1_boot", fresh=False, extra=["--pin_low=gp_a"])
    c0 = count_a(s)
    s.send("sim pin gp_a 1")
    time.sleep(0.5)
    s.send("sim pulse gp_a 20 100")
    time.sleep(6)
    n = count_a(s) - c0
    result("H1.6", n == 21, f"boot with line active, release + 20 pulses -> {n} (21 = trailing edge of the boot pulse + 20)")
    s.stop()


# --- H2 RATE on live counters (D2) ----------------------------------------
def h2_one(sample):
    s = start(f"h2_{sample}", air=True)
    air = Proc(f"h2_{sample}_air", [sys.executable, "-u", str(sim_dir / "sim_air.py"), "--appkey", APPKEY], logd)
    try:
        am = air.mark()
        save(s, [f"config radio-deveui {DEVEUI}", f"config radio-appkey {APPKEY}",
                 "config radio-mode lorawan", "config cap-gpio-mode inputs",
                 "config cap-gpio-input-a digital", "config input-a-counter true",
                 f"config interval-sample {sample}"])
        air.expect(rf"{DEVEUI} FCnt \d+ FPort 2 ", 120, am)
        time.sleep(3)
        ask(s, "alarm new mb input-a-count hi 3 dwell 10", r".")
        save(s, [])
        time.sleep(15)
        am = air.mark()
        trans, last = [], 0
        t0 = time.monotonic()
        s.send("sim pulse gp_a 5 500")
        # 60 s of >= 3 edges per 10 s (one pulse every 2 s), then stop
        nxt = t0 + 4
        while time.monotonic() - t0 < 100:
            now = time.monotonic()
            if 5 < now - t0 < 66 and now >= nxt:
                s.send("sim pulse gp_a 1 100")
                nxt = now + 2
            a = alarm_active(s)
            a = a[0] if a else last
            if a != last:
                trans.append((round(now - t0), a))
                last = a
            time.sleep(0.5)
        frames = [l for l in air.lines[am:] if "FPort 3 " in l or '"alarm' in l]
        ok = [t[1] for t in trans] == [1, 0] and trans[0][0] <= 12 and 66 <= trans[1][0] <= 82
        result(f"H2 sample={sample}", ok,
               f"transitions {trans} (want activate <=12 s, one deactivate ~66-80 s); air alarm lines {len(frames)}")
    finally:
        s.stop(); air.stop()


def h2():
    h2_one(60)
    h2_one(0)


# --- H3 stillness with capability off (D1) --------------------------------
def h3():
    s = start("h3")
    save(s, ["config cap-gpio-mode none", "config cap-accelerometer false"])
    ask(s, "alarm new mb pir-motion from 0 to 0 dwell 30", r".")
    ask(s, "alarm new mb accel-motion from 0 to 0 dwell 30", r".")
    save(s, [])
    seen = set()
    t0 = time.monotonic()
    while time.monotonic() - t0 < 150:
        seen.update(alarm_active(s)); time.sleep(5)
    buz = ask(s, "sim pin gp_b", r"out_edges=(\d+)").group(1)
    result("H3.1-3", seen == {0} and buz == "0",
           f"capability off, 150 s: active values seen {sorted(seen)}, buzzer edges {buz}")
    s.stop()
    # PIR on: gp_a low = no motion
    s = start("h3", fresh=False, extra=["--pin_low=gp_a"])
    save(s, ["config cap-gpio-mode pir"])
    s.send("sim pin gp_a 0")
    t0, act_at = time.monotonic(), None
    while time.monotonic() - t0 < 120 and act_at is None:
        a = alarm_active(s)
        if a and a[0] == 1:
            act_at = round(time.monotonic() - t0)
        time.sleep(1)
    s.send("sim pin gp_a 1"); time.sleep(1); s.send("sim pin gp_a 0")
    clr = False
    t1 = time.monotonic()
    while time.monotonic() - t1 < 20 and not clr:
        a = alarm_active(s)
        clr = bool(a) and a[0] == 0
        time.sleep(1)
    result("H3.4 pir", act_at is not None and clr,
           f"pir mode: stillness activated after {act_at} s (30 s PIR warm-up + 30 s dwell), cleared on motion={clr}")
    s.stop()
    # accelerometer on
    s = start("h3", fresh=False)
    save(s, ["config cap-gpio-mode none", "config cap-accelerometer true",
             "config accel-motion-sensitivity high"])
    t0, act_at = time.monotonic(), None
    while time.monotonic() - t0 < 90 and act_at is None:
        a = alarm_active(s)
        if len(a) > 1 and a[1] == 1:
            act_at = round(time.monotonic() - t0)
        time.sleep(1)
    s.send("sim accel shake")
    clr = False
    t1 = time.monotonic()
    while time.monotonic() - t1 < 20 and not clr:
        a = alarm_active(s)
        clr = len(a) > 1 and a[1] == 0
        time.sleep(1)
    result("H3.4 accel", act_at is not None and clr,
           f"accel on: stillness activated after {act_at} s, cleared on shake={clr}")
    s.stop()


# --- H4 11 B tier (D3) ----------------------------------------------------
def h4():
    s = start("h4")
    save(s, ["config lrw-region us915", "config cap-sht true", "config cap-barometer true",
             "config cap-w1-sensors true"])
    s.send("sim sht off")
    m = s.mark()
    s.send("w1 enroll")
    s.expect(r"auto-enrolled dallas", 30, m)
    s.send("ats sensors sample"); time.sleep(3)
    m = s.mark()
    s.send("ats radio compose 11")
    time.sleep(3)
    frames = [(int(x.group(2)), x.group(3)) for l in s.lines[m:]
              for x in [re.search(r"frame (\d+) \((\d+) B\): ([0-9a-f]+)", l)] if x]
    log = "\n".join(s.lines[m:])
    dec = subprocess.run(["node", "-e", f"""
const d=require({json.dumps(str(DECODER))});
for (const h of {json.dumps([f[1] for f in frames])}) {{
  const r=d.decodeUplink({{fPort:2,bytes:[...Buffer.from(h,'hex')]}});
  console.log(JSON.stringify(r.errors.length?{{err:r.errors}}:r.data));
}}"""], capture_output=True, text=True)
    decoded = [json.loads(x) for x in dec.stdout.split("\n") if x]
    sizes = [f[0] for f in frames]
    pc = [d.get("page_count") for d in decoded]
    errs = [d for d in decoded if "err" in d]
    keys = [sorted(k for k, v in d.items() if v is not None and k not in ("page_index", "page_count")) for d in decoded]
    ok = (frames and max(sizes) <= 11 and not errs and "EMSGSIZE" not in log
          and all(p in (None, len(frames)) for p in pc))
    result("H4.1-3", ok, f"{len(frames)} frames sizes {sizes}, page_count {pc}, decode errors {len(errs)}")
    print("   fields per page:", keys)
    wrn = [l for l in s.lines if "<wrn>" in l and ("NA" in l or "skip" in l.lower() or "1-Wire" in l)]
    print("   WRN:", wrn[-6:])
    s.stop()


# --- H5 settings save reboots over NFC and radio (M3 smoke) ---------------
def h5():
    s = Proc("h5", [exe, f"--flash={logd/'h5.bin'}"], logd)
    (logd / "h5.bin").unlink(missing_ok=True)
    air = Proc("h5_air", [sys.executable, "-u", str(sim_dir / "sim_air.py"), "--appkey", APPKEY], logd)
    try:
        s.expect(BOOT, 30); time.sleep(1.5)
        am = air.mark()
        save(s, [f"config radio-deveui {DEVEUI}", f"config radio-appkey {APPKEY}",
                 "config radio-mode lorawan", f"config secret-key {APPKEY}"])
        air.expect(rf"{DEVEUI} FCnt \d+ FPort 2 ", 120, am)
        time.sleep(4)
        m = s.mark()
        out = subprocess.run([sys.executable, str(sim_dir / "sim_nfc.py"), "cmd", '{"settings_save": {}}'],
                             capture_output=True, text=True, timeout=60)
        try:
            s.expect(BOOT, 30, m); rb = True
        except TimeoutError:
            rb = False
        result("H5 nfc", rb, f"NFC settings_save -> reboot={rb} ({out.stdout.strip()[-80:]})")
        am = air.mark()
        air.expect(rf"{DEVEUI} FCnt \d+ FPort 2 ", 120, am)
        time.sleep(4)
        m = s.mark()
        air.send("down * 85 08013200")
        s.send("ats radio check")
        try:
            s.expect(BOOT, 60, m); rb = True
        except TimeoutError:
            rb = False
        result("H5 radio", rb, f"downlink settings_save -> reboot={rb}")
    finally:
        s.stop(); air.stop()


# --- H6 v1.4.x upgrade: 17 B alarm-N keys (D6) ----------------------------
def h6():
    s = start("h6")
    save(s, [f"config radio-deveui {DEVEUI}", f"config radio-appkey {APPKEY}",
             "config lrw-joineui 70b3d57ed0000001", f"config secret-key {APPKEY}",
             "config interval-report 600", "config cap-barometer true"])
    m = s.mark(); s.send("config show"); time.sleep(2)
    before = [l for l in s.lines[m:] if l.startswith("config ")]
    for i in range(16):
        s.send(f"sim nvs write config/alarm-{i} 0102030405060708090a0b0c0d0e0f1011")
    time.sleep(1)
    boots = []
    for b in range(4):
        m = s.mark()
        s.send("ats device reboot")
        s.expect(BOOT, 30, m + 1); time.sleep(3)
        seg = s.lines[m:]
        boots.append((sum("stale" in l and "expected" in l for l in seg),
                      sum("Stale key" in l and "removed" in l and "not" not in l for l in seg),
                      sum("set-value" in l or "error(-22)" in l for l in seg)))
    m = s.mark(); s.send("config show"); time.sleep(2)
    after = [l for l in s.lines[m:] if l.startswith("config ")]
    diff = [a for a, b in zip(before, after) if a != b]
    stale = sum(b[0] for b in boots); removed = sum(b[1] for b in boots)
    ok = (stale == 24 and removed == 16 and all(b[2] == 0 for b in boots)
          and boots[-1][:2] == (0, 0) and not diff and len(before) == len(after))
    result("H6", ok, f"per boot (stale, removed, set-value err): {boots}; config diff {diff}")
    s.stop()


# --- H7 buzzer and PIR modes ----------------------------------------------
def h7():
    s = start("h7")
    save(s, ["config cap-gpio-mode buzzer", "config alarm-buzzer-mode once",
             "config cap-light-sensor true", "config interval-sample 5"])
    ask(s, "alarm new mb illuminance lo 0 hi 1000 dwell 0", r".")
    save(s, [])
    e0 = int(ask(s, "sim pin gp_b", r"out_edges=(\d+)").group(1))
    s.send("sim light 5000")
    time.sleep(20)
    e1 = int(ask(s, "sim pin gp_b", r"out_edges=(\d+)").group(1))
    act = alarm_active(s)
    result("H7 buzzer", act[:1] == [1] and e1 > e0, f"light alarm active={act[:1]}, buzzer edges {e0}->{e1}")
    s.stop()
    s = start("h7", fresh=False, extra=["--pin_low=gp_a"])
    ask(s, "alarm clear all", r".")
    save(s, ["config cap-gpio-mode pir", "config cap-light-sensor false"])
    ask(s, "alarm new mb pir-motion from 0 to 1 dwell 0", r".")
    save(s, [], reboot=True)
    s.send("sim pin gp_a 0")
    time.sleep(33)
    s.send("sim pin gp_a 1"); time.sleep(1); s.send("sim pin gp_a 0")
    time.sleep(3)
    mc = ask(s, "ats sensors sample", r"motion-count[:=]\s*(\d+)")
    acts = [l for l in s.lines if re.search(r"<(inf|wrn)> app_alarm: Alarm uplink", l)]
    result("H7 pir", int(mc.group(1)) >= 1 and len(acts) >= 1,
           f"motion-count {mc.group(1)}, pir-motion activation log lines {len(acts)}: {acts[:1]}")
    s.stop()


TESTS = {"h1": h1, "h2": h2, "h3": h3, "h4": h4, "h5": h5, "h6": h6, "h7": h7}
for k, f in TESTS.items():
    if only and k not in only:
        continue
    try:
        f()
    except Exception as e:
        result(k.upper(), False, f"exception {type(e).__name__}: {e}")
print("\n".join(f"{r[1]} {r[0]}: {r[2]}" for r in RESULTS))
print(f"logs: {logd}")
sys.exit(0 if RESULTS and all(r[1] == "PASS" for r in RESULTS) else 1)
