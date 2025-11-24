# ToDo List

- Check how to implement NFC factory reset properly (to preserve serial number, etc.).

- Allow setting the public LoRaWAN network type in runtime (today this is hard-coded in the LoRaWAN subsystem, and can only be chosen at compile time).

- Finish accelerometer tilt detection (with options CONFIG_LIS2DH_ACCEL_HP_FILTERS=y and CONFIG_LIS2DH_TRIGGER_GLOBAL_THREAD=y ???).

- Implement optional activation / deactivation delay for hall switches

  - Activate calibration after 5-second activation of both hall switches.

# Development Notes

## Zephyr I²C references

The project references the following Zephyr discussions/patches related to I²C behavior:

* [https://github.com/zephyrproject-rtos/zephyr/issues/37414](https://github.com/zephyrproject-rtos/zephyr/issues/37414)
* [https://github.com/dinuxbg/zephyr/commit/aaf9c4ee246ee306f758ba0055c3c367a8ca53be](https://github.com/dinuxbg/zephyr/commit/aaf9c4ee246ee306f758ba0055c3c367a8ca53be)
* [https://github.com/zephyrproject-rtos/zephyr/pull/72805](https://github.com/zephyrproject-rtos/zephyr/pull/72805)

## Zephyr ADC references

For ADC behavior and fixes, see:

* [https://github.com/sense-Jo/zephyr/commit/5c476cc087cc685f4b85a0f15dc395b53154340c](https://github.com/sense-Jo/zephyr/commit/5c476cc087cc685f4b85a0f15dc395b53154340c)
* [https://github.com/sense-Jo/zephyr/commit/a1cf2447696f5d76f85445362af07abf03afc24d](https://github.com/sense-Jo/zephyr/commit/a1cf2447696f5d76f85445362af07abf03afc24d)

These links provide context for certain workarounds or configuration choices that may appear in the code.
