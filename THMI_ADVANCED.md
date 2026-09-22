# T-HMI Advanced firmware

This fork adds a touch-first Control Center for the LilyGo T-HMI and makes it the board's home
screen. Other supported devices keep the stock Tactility launcher.

The T-HMI profile uses 120 MHz flash and PSRAM clocks with ESP-IDF 6.1 for maximum performance.

## Included shortcuts

- Apps (complete application list)
- Files (SD-card file browser)
- Settings

Other apps (Wi-Fi, Bluetooth, Bluetooth Remote, Desk Clock, 4G Gateway) are opened from Apps.
Shortcuts whose app is not registered are automatically omitted instead of showing dead buttons.

## 4G Gateway app — recent changes

The 4G Gateway app talks to the paired N0D3 DeskOS gateway (`esp32-s3-a-sim7670x-4g`,
firmware v0.3.0+) over its authenticated HTTP control plane, not WebSocket.

- **Status tab**: signal-history sparkline (last ~60 CSQ readings, fetched every 20s from
  `/api/node/csq_history`), and a summary line for engines that already run on the gateway but
  were previously WebUI-only — MQTT connection, Telegram configuration, automation rule count
  (with a visible "ATENTIE" warning when a rule is currently latched, or gateway battery drops
  below 15%), and geofence zone count (with "in zona" when inside one).
- **Config tab** (new): edit the gateway's Wi-Fi STA credentials, cellular APN, MQTT
  broker (URI/user/password) and Telegram bot (token/chat ID) directly from the device — no
  laptop needed next to the gateway. Add a new automation rule (trigger/threshold/action) or a
  new geofence zone (using the gateway's current GNSS fix, no manual coordinate entry). Existing
  rules and zones are listed with their id/index; delete either by entering that id/index.
- Traffic counters (`RX`/`TX` on the Status tab) are lwIP **packet** counts, global across the
  gateway's WiFi AP + STA + PPP interfaces combined — not bytes, and not 4G-only. The gateway has
  no per-interface PPP byte counter without a netif-level hook, an area intentionally left alone
  (see `docs/PERFORMANCE.md` in the gateway's own repository).

## Desk Clock — configurable weather location

Desk Clock defaults to Bucharest (Open-Meteo forecast + local timezone), same as before. Tap the
gear icon in its toolbar to reveal a city name + latitude/longitude form and save a different
location; it's persisted on-device and the forecast timezone is derived from the coordinates
(`timezone=auto`) instead of being hardcoded to `Europe/Bucharest`.

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
