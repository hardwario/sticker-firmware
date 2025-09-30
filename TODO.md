- Check how to implement NFC factory reset properly (to preserve serial number, etc.).

- Allow setting the public LoRaWAN network type in runtime (today this is hard-coded in the LoRaWAN subsystem, and can only be chosen at compile time).

- Finish accelerometer tilt detection (with options CONFIG_LIS2DH_ACCEL_HP_FILTERS=y and CONFIG_LIS2DH_TRIGGER_GLOBAL_THREAD=y ???).

- Implement optional activation / deactivation delay for hall switches
