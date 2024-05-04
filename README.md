# sticker

Create the `sticker` directory:

    mkdir sticker

Switch to the `sticker` directory:

    cd sticker

Create Python virtual environment:

    python -m venv .venv

Activate the virtual environment:

    source .venv/bin/activate

Install the **West** tool:

    pip install west

Initialize the **West** workspace:

    west init -m git@github.com:hardwario/sticker.git

Update the **West** workspace:

    west update

Install required Python packages:

    pip install -r zephyr/scripts/requirements.txt -r bootloader/mcuboot/scripts/requirements.txt

Build the application in production mode:

    make

Build the application in debug mode:

    make debug

Flash the application:

    make flash

Clean build outputs:

    make clean

Start RTT logging:

    JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 4000 -RTTAddress 0x20000800 -RTTChannel 1 ~/.sticker.log

Connect to shell:

    nc localhost 19021

Read logs:

    tail -f tail -f ~/.sticker.log

Determine RTT block address:

    readelf -s build/zephyr/zephyr.elf | grep _SEGGER_RTT
