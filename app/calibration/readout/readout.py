import re
import sys
import time

import pylink
import schedule

reading = {}

def print_job():
    global reading

    print("=" * 120)
    for device_serial_no, (localtime, jlink_serial_no, temperature, humidity, ) in reading.items():
        print("Time: " + localtime + " / J-Link: " + str(jlink_serial_no) +  " / Serial number: " + str(device_serial_no) + " / Temperature: " + temperature + " C / Humidity: " + str(humidity) + " %")

def main():
    global reading

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
            jlink.rtt_start(block_address=0x20000800)
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
                    match = re.match(r"Serial number: (\d{10}) / Temperature: (-?\d+\.\d+) C / Humidity: (\d+\.\d+) %", line)
                    if match:
                        device_serial_no, temperature, humidity = match.groups()
                        reading[device_serial_no] = (time.strftime("%Y-%m-%d %H:%M:%S"), jlink_serial_no, temperature, humidity, )

                time.sleep(0.01)
    except:
        print("Communication error.")
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
