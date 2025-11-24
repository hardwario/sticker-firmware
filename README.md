# sticker

Create the `sticker` directory:

    mkdir sticker

Switch to the `sticker` directory:

    cd sticker

Create Python virtual environment:

    python -m venv .venv

Activate the virtual environment:

    source .venv/bin/activate

Upgrade `pip` tool:

    pip install --upgrade pip

Install the **West** tool:

    pip install west

Initialize the **West** workspace:

    west init -m git@github.com:hardwario/sticker.git

Update the **West** workspace:

    west update

Export a **Zephyr CMake package**:

    west zephyr-export

Install required **Python packages**:

    west packages pip --install

Install the **RTTT** tool:

    pip install rttt

Install the **Protocol Buffers** dependencies:

    pip install protobuf grpcio-tools

Install the appropriate version of the **Zephyr SDK**:

    west sdk install

Switch to the `app` directory:

    cd app

Build the application in production mode:

    make

Build the application in debug mode:

    make debug

Flash the application:

    make flash

Clean build outputs:

    make clean

Start the RTT terminal:

    make rttt

Determine RTT block address:

    readelf -s build/zephyr/zephyr.elf | grep _SEGGER_RTT

Format the source codes:

    make format

## Remarks

I2C:
https://github.com/zephyrproject-rtos/zephyr/issues/37414
https://github.com/dinuxbg/zephyr/commit/aaf9c4ee246ee306f758ba0055c3c367a8ca53be
https://github.com/zephyrproject-rtos/zephyr/pull/72805

ADC:
https://github.com/sense-Jo/zephyr/commit/5c476cc087cc685f4b85a0f15dc395b53154340c
https://github.com/sense-Jo/zephyr/commit/a1cf2447696f5d76f85445362af07abf03afc24d

## License

This project is licensed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0) - see the [LICENSE](LICENSE) file for details.

---

Made with &#x2764;&nbsp; by [**HARDWARIO a.s.**](https://www.hardwario.com/) in the heart of Europe.
