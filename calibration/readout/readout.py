import re
import sys
import time

import pylink

try:
    jlink = pylink.JLink()
    jlink.open()
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(chip_name='STM32WLE5CC', speed=4000)
    jlink.rtt_start(block_address=0x20000800)
except KeyboardInterrupt:
    sys.exit(0)
except:
    print("Failed to connect to the target.")
    sys.exit(1)

try:
    terminal_str = ""
    while jlink.connected():
        terminal_bytes = jlink.rtt_read(1, 1024)
        if terminal_bytes:
            terminal_str += "".join(map(chr, terminal_bytes))
        lines = terminal_str.split("\n")
        terminal_str = lines[-1]
        for line in lines[:-1]:
            print(line)
        time.sleep(0.01)
except KeyboardInterrupt:
    sys.exit(0)
except:
    print("Communication error.")
    sys.exit(1)
