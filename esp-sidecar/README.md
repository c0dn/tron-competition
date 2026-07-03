# esp-sidecar

ESP32-C3 companion firmware for the TRON competition project. Pairs with the
micro:bit firmware in [`../microbit`](../microbit).

Current firmware is a minimal FreeRTOS bring-up that prints over the console —
enough to verify the build/flash/monitor toolchain on the board.

## Target

ESP32-C3 SuperMini (4 MB flash), native USB Serial/JTAG over USB-C — flashing
and the serial console share the one port.

The console is routed to the USB Serial/JTAG controller
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`); without it,
`idf.py monitor` over the USB port shows no output.

## Requirements

- ESP-IDF **v5.5.1** (the version this project is built against).

Activate the ESP-IDF environment in your shell before building — see the
[ESP-IDF getting-started guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c3/get-started/index.html)
(typically `. $IDF_PATH/export.sh`, or the equivalent for your shell).

## Build, flash & monitor

```bash
idf.py set-target esp32c3     # first time only
idf.py build
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the board's serial device (e.g. `/dev/ttyACM0`). Exit the
monitor with `Ctrl-]`. If the board does not enumerate, force download mode:
hold BOOT → tap RST → release BOOT.
