# T-HMI Advanced firmware

This fork adds a touch-first Control Center for the LilyGo T-HMI and makes it the board's home
screen. Other supported devices keep the stock Tactility launcher.

The T-HMI profile uses 120 MHz flash and PSRAM clocks with ESP-IDF 6.1 for maximum performance.

## Included shortcuts

- Wi-Fi management
- Bluetooth management
- SD-card file browser
- Complete application list
- Settings
- System information
- I2C scanner
- USB settings, when the USB MSC service is available

Unavailable services are automatically omitted instead of showing dead buttons.

## Build and flash

Install the ESP-IDF version required by the upstream Tactility revision, then run from the project
root:

```sh
python device.py lilygo-thmi
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the serial port exposed by the T-HMI. Hold BOOT while connecting USB if the
board does not enter download mode automatically.

## Restore the stock launcher

Change `apps.launcherAppId` in `Devices/lilygo-thmi/device.properties` back to
`tactility.launcher`.
