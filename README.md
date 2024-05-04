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

Set default board name:

    west config build.board sticker

Update the **West** workspace:

    west update

Install required Python packages:

    pip install -r zephyr/scripts/requirements.txt -r bootloader/mcuboot/scripts/requirements.txt

Build application:

    west build -b sticker

Start RTT logging:

    JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 4000 -RTTChannel 1 ~/.sticker.log

Connect to shell:

    nc localhost 19021

Determine RTT block address:

    readelf -s build/zephyr/zephyr.elf | grep _SEGGER_RTT
