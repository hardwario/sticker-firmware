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

I2C:
https://github.com/zephyrproject-rtos/zephyr/issues/37414
https://github.com/dinuxbg/zephyr/commit/aaf9c4ee246ee306f758ba0055c3c367a8ca53be
https://github.com/zephyrproject-rtos/zephyr/pull/72805

ADC:
https://github.com/sense-Jo/zephyr/commit/5c476cc087cc685f4b85a0f15dc395b53154340c
https://github.com/sense-Jo/zephyr/commit/a1cf2447696f5d76f85445362af07abf03afc24d
