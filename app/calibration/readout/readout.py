import os
import re
import subprocess
import sys
import time

import pylink
import schedule

# Path to the ELF file (relative to this script)
ELF_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "build", "zephyr", "zephyr.elf")

SENTINEL = 32767

reading = {}


def get_rtt_address(elf_path):
    """Extract _SEGGER_RTT address from ELF using nm."""
    try:
        result = subprocess.run(
            ["arm-none-eabi-nm", elf_path],
            capture_output=True, text=True, check=True,
        )
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == "_SEGGER_RTT" and parts[1] == "B":
                return int(parts[0], 16)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return None


def fmt_val(raw, scale=100, unit="C", fmt=".2f"):
    """Format a raw int16 x100 value, or '-' for SENTINEL."""
    if raw == SENTINEL:
        return "-"
    return f"{raw / scale:{fmt}} {unit}"


def print_job():
    global reading

    print("=" * 140)
    for sn, r in reading.items():
        print(f"Time: {r['time']} / J-Link: {r['jlink']} / SN: {sn} / "
              f"Uptime: {r['uptime']}s / "
              f"SHT: {fmt_val(r['st'])} {fmt_val(r['sh'], unit='%', fmt='.1f')} / "
              f"Hygro: {fmt_val(r['ht'])} {fmt_val(r['hh'], unit='%', fmt='.1f')} / "
              f"T1: {fmt_val(r['t1'])} T2: {fmt_val(r['t2'])} / "
              f"P1: {fmt_val(r['p1t'])} {fmt_val(r['p1h'], unit='%', fmt='.1f')} / "
              f"P2: {fmt_val(r['p2t'])} {fmt_val(r['p2h'], unit='%', fmt='.1f')}")


RTT_PATTERN = re.compile(
    r"SN:(\d{10}) UP:(\d+) ST:(-?\d+) SH:(-?\d+) "
    r"HT:(-?\d+) HH:(-?\d+) T1:(-?\d+) T2:(-?\d+) "
    r"P1T:(-?\d+) P1H:(-?\d+) P2T:(-?\d+) P2H:(-?\d+)"
)


def main():
    global reading

    rtt_addr = get_rtt_address(os.path.realpath(ELF_PATH))
    if rtt_addr is None:
        print("Failed to read _SEGGER_RTT address from ELF: " + ELF_PATH)
        sys.exit(1)
    print(f"RTT control block address: 0x{rtt_addr:08X}")

    schedule.every(1).seconds.do(print_job)

    try:
        jlink = pylink.JLink()
        emulators = jlink.connected_emulators()
        sessions = []
        for emulator in emulators:
            jlink_serial_no = emulator.SerialNumber
            jlink = pylink.JLink()
            jlink.open(serial_no=jlink_serial_no)
            jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
            jlink.connect(chip_name='STM32WLE5CC', speed=4000)
            jlink.rtt_start(block_address=rtt_addr)
            sessions.append((jlink, jlink_serial_no))
    except:
        print("Failed to connect to the target.")
        sys.exit(1)

    try:
        terminal_str = {}
        for (jlink, jlink_serial_no) in sessions:
            terminal_str[jlink_serial_no] = ""
        while True:
            schedule.run_pending()
            for (jlink, jlink_serial_no) in sessions:
                if not jlink.connected():
                    continue
                terminal_bytes = jlink.rtt_read(1, 1024)
                if terminal_bytes:
                    terminal_str[jlink_serial_no] += "".join(map(chr, terminal_bytes))
                lines = terminal_str[jlink_serial_no].split("\n")
                terminal_str[jlink_serial_no] = lines[-1]
                for line in lines[:-1]:
                    match = RTT_PATTERN.match(line)
                    if match:
                        g = match.groups()
                        reading[g[0]] = {
                            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                            "jlink": jlink_serial_no,
                            "uptime": int(g[1]),
                            "st": int(g[2]), "sh": int(g[3]),
                            "ht": int(g[4]), "hh": int(g[5]),
                            "t1": int(g[6]), "t2": int(g[7]),
                            "p1t": int(g[8]), "p1h": int(g[9]),
                            "p2t": int(g[10]), "p2h": int(g[11]),
                        }

                time.sleep(0.01)
    except:
        print("Communication error.")
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
