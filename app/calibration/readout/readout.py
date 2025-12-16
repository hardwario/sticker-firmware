#!/usr/bin/env python3
"""
STICKER Calibration RTT Readout Script

Reads calibration data from multiple J-Link connected devices via RTT
and outputs CSV format with sensor serial numbers in header.
"""

import argparse
import re
import sys
import time
from datetime import datetime

import pylink

# Current reading state per J-Link
# Structure: {jlink_sn: {sticker_sn: {sensor_type: {sn, temp, humidity}}}}
current_readings = {}

# Sensor order for consistent CSV columns
SENSOR_ORDER = ["Sticker", "SHT4x", "DS18B20-1", "DS18B20-2", "MP-1", "MP-1-SHT", "MP-2", "MP-2-SHT"]


def parse_line(line, jlink_serial_no):
    """Parse RTT output line and store reading."""
    global current_readings

    if jlink_serial_no not in current_readings:
        current_readings[jlink_serial_no] = {}

    # Sticker SN: 1234567890 (only SN, no values)
    match = re.match(r"Sticker SN: (\d+)$", line)
    if match:
        sticker_sn = match.group(1)
        if sticker_sn not in current_readings[jlink_serial_no]:
            current_readings[jlink_serial_no][sticker_sn] = {}
        current_readings[jlink_serial_no][sticker_sn]["Sticker"] = {
            "sn": sticker_sn, "temp": None, "humidity": None
        }
        return sticker_sn

    # SHT4x SN: 1234567890 / T: 25.50 C / H: 45.30 %
    match = re.match(r"SHT4x SN: (\d+) / T: (-?\d+\.\d+) C / H: (\d+\.\d+) %", line)
    if match:
        sn, temp, humidity = match.groups()
        for sticker_sn in current_readings[jlink_serial_no]:
            current_readings[jlink_serial_no][sticker_sn]["SHT4x"] = {
                "sn": sn, "temp": temp, "humidity": humidity
            }
        return None

    # DS18B20-N SN: 281234567890ABCD / T: 24.80 C
    match = re.match(r"DS18B20-(\d+) SN: (\d+) / T: (-?\d+\.\d+) C", line)
    if match:
        idx, sn, temp = match.groups()
        for sticker_sn in current_readings[jlink_serial_no]:
            current_readings[jlink_serial_no][sticker_sn][f"DS18B20-{idx}"] = {
                "sn": sn, "temp": temp, "humidity": None
            }
        return None

    # MP-N SN: 191234567890ABCD (only SN, no values)
    match = re.match(r"MP-(\d+) SN: (\d+)$", line)
    if match:
        idx, sn = match.groups()
        for sticker_sn in current_readings[jlink_serial_no]:
            current_readings[jlink_serial_no][sticker_sn][f"MP-{idx}"] = {
                "sn": sn, "temp": None, "humidity": None
            }
        return None

    # MP-N-SHT SN: 1234567890 / T: 25.20 C / H: 44.50 %
    match = re.match(r"MP-(\d+)-SHT SN: (\d+) / T: (-?\d+\.\d+) C / H: (\d+\.\d+) %", line)
    if match:
        idx, sn, temp, humidity = match.groups()
        for sticker_sn in current_readings[jlink_serial_no]:
            current_readings[jlink_serial_no][sticker_sn][f"MP-{idx}-SHT"] = {
                "sn": sn, "temp": temp, "humidity": humidity
            }
        return None

    return None


def write_sensor_list(jlink_serial_no, sensors, outfile):
    """Write sensor list with Device;SN format at the beginning."""
    print("Device;SN", file=outfile)
    print("Device;SN")
    print(f"J-Link;{jlink_serial_no}", file=outfile)
    print(f"J-Link;{jlink_serial_no}")

    for sensor_type in SENSOR_ORDER:
        if sensor_type in sensors:
            sn = sensors[sensor_type]["sn"]
            print(f"{sensor_type};{sn}", file=outfile)
            print(f"{sensor_type};{sn}")

    # Empty line separator
    print("", file=outfile)
    print("")


def get_csv_header(sensors):
    """Generate CSV header for data columns."""
    header = ["Timestamp"]
    for sensor_type in SENSOR_ORDER:
        if sensor_type in sensors:
            temp = sensors[sensor_type]["temp"]
            humidity = sensors[sensor_type]["humidity"]

            if temp is None and humidity is None:
                # SN-only sensor (Sticker, MP-N) - no data columns
                pass
            elif humidity is not None:
                # T+H sensor
                header.append(f"{sensor_type} T[C]")
                header.append(f"{sensor_type} H[%]")
            else:
                # T-only sensor (DS18B20)
                header.append(f"{sensor_type} T[C]")
    return header


def get_csv_row(sensors):
    """Generate CSV data row with sensor values."""
    row = [datetime.now().strftime("%Y-%m-%d %H:%M:%S")]
    for sensor_type in SENSOR_ORDER:
        if sensor_type in sensors:
            temp = sensors[sensor_type]["temp"]
            humidity = sensors[sensor_type]["humidity"]

            if temp is None and humidity is None:
                # SN-only sensor - no value column
                pass
            elif humidity is not None:
                # T+H sensor
                row.append(temp)
                row.append(humidity)
            else:
                # T-only sensor
                row.append(temp)
    return row


def main():
    parser = argparse.ArgumentParser(description="STICKER Calibration RTT Readout")
    parser.add_argument("-o", "--output", help="Output CSV file (overrides auto-generated filename)")
    parser.add_argument("-i", "--interval", type=int, default=1, help="Output interval in seconds (default: 1)")
    parser.add_argument("-d", "--header-delay", type=int, default=3, help="Delay before writing header to detect all sensors (default: 3)")
    args = parser.parse_args()

    header_written = {}
    start_time = {}
    outfiles = {}  # Per-sticker output files {header_key: file_handle}

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
            jlink.rtt_start(block_address=0x20000800)
            sessions.append((jlink, jlink_serial_no))
            print(f"# Connected to J-Link: {jlink_serial_no}", file=sys.stderr)
    except Exception as e:
        print(f"Failed to connect to the target: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        terminal_str = {}
        last_output = {}
        for (jlink, jlink_serial_no) in sessions:
            terminal_str[jlink_serial_no] = ""
            last_output[jlink_serial_no] = 0

        while True:
            for (jlink, jlink_serial_no) in sessions:
                if not jlink.connected():
                    continue

                terminal_bytes = jlink.rtt_read(1, 1024)
                if terminal_bytes:
                    terminal_str[jlink_serial_no] += "".join(map(chr, terminal_bytes))

                lines = terminal_str[jlink_serial_no].split("\n")
                terminal_str[jlink_serial_no] = lines[-1]

                for line in lines[:-1]:
                    parse_line(line.strip(), jlink_serial_no)

                # Output CSV at interval
                now = time.time()
                if now - last_output[jlink_serial_no] >= args.interval:
                    if jlink_serial_no in current_readings:
                        for sticker_sn, sensors in current_readings[jlink_serial_no].items():
                            if sensors:
                                header_key = f"{jlink_serial_no}_{sticker_sn}"

                                # Track when sticker was first seen
                                if header_key not in start_time:
                                    start_time[header_key] = now

                                # Wait for header delay before writing (to detect all sensors)
                                if now - start_time[header_key] < args.header_delay:
                                    continue

                                # Create output file on first write
                                if header_key not in outfiles:
                                    if args.output:
                                        filename = args.output
                                    else:
                                        filename = f"sticker-{sticker_sn}.csv"
                                    outfiles[header_key] = open(filename, "w", newline="")
                                    print(f"# Writing to: {filename}", file=sys.stderr)

                                outfile = outfiles[header_key]

                                # Write sensor list and header once per sticker
                                if header_key not in header_written:
                                    # First: sensor list (Device;SN)
                                    write_sensor_list(jlink_serial_no, sensors, outfile)

                                    # Then: data header
                                    header = get_csv_header(sensors)
                                    print(";".join(header), file=outfile)
                                    print(";".join(header))
                                    header_written[header_key] = True

                                # Write data row
                                row = get_csv_row(sensors)
                                print(";".join(row), file=outfile)
                                print(";".join(row))
                                outfile.flush()

                    last_output[jlink_serial_no] = now

            time.sleep(0.01)

    except Exception as e:
        print(f"Communication error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        for outfile in outfiles.values():
            outfile.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
