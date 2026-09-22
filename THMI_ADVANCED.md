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

## 4G Gateway app

The 4G Gateway app talks to the paired N0D3 DeskOS gateway (`esp32-s3-a-sim7670x-4g`,
firmware v0.3.3+) over its authenticated HTTP control plane, not WebSocket
(see that repo's `docs/MODULES.md` for the exact `/api/node/*` surface).

- **Status tab**: real packet-count traffic (`RX`/`TX` — lwIP counts, global across the gateway's
  WiFi AP + STA + PPP interfaces combined, not bytes and not 4G-only; the gateway has no
  per-interface PPP byte counter without a netif-level hook, an area intentionally left alone,
  see that repo's `docs/PERFORMANCE.md`). Signal-history sparkline (last ~60 CSQ readings,
  fetched every 20s). Summary line for engines that already ran on the gateway but were
  previously WebUI-only: MQTT connection, Telegram configuration, automation rule count (with a
  visible "ATENTIE" warning when a rule is latched, or gateway battery drops below 15%), geofence
  zone count ("in zona" when inside one), host watchdog status, Wake-on-LAN target.
- **Config tab**: edit the gateway's Wi-Fi STA credentials, cellular APN, MQTT broker
  (URI/user/password) and Telegram bot (token/chat ID) directly from the device — no laptop
  needed next to the gateway. Add/list/delete automation rules (trigger/threshold/action),
  geofence zones (using the gateway's current GNSS fix, no manual coordinate entry), and
  scheduled events ("in N minutes" instead of a raw Unix timestamp — needs the T-HMI's own clock
  to be correct, see Settings → Time & Date below). Configure and view the host watchdog
  (availability monitor → SMS alert). Send a Wake-on-LAN magic packet.

## Desk Clock — configurable weather location

Desk Clock defaults to Bucharest (Open-Meteo forecast + local timezone), same as before. Tap the
gear icon in its toolbar to reveal a city name + latitude/longitude form and save a different
location; it's persisted on-device and the forecast timezone is derived from the coordinates
(`timezone=auto`) instead of being hardcoded to `Europe/Bucharest`.

## Settings — per-app icons

Upstream's `AppManifest` no longer carries a per-app icon (removed in a kernel refactor), so
every row in Settings used to show the same generic icon. `Settings.cpp` now has a small,
file-local `manifest->id → icon` lookup table (doesn't touch `AppManifest` itself, so it's safe
across upstream syncs) — every built-in Settings entry gets a distinct, recognizable icon.

## Timezone: the "an hour behind" trap

If the clock looks correct-ish but off by exactly one hour, it's very likely a timezone the user
never set: Tactility's own default is `Europe/Amsterdam` (CET/CEST), one hour behind Romania.
Also, the timezone picker (Settings → Time & Date → Timezone) only lists the **first ~50** zones
unfiltered — `Europe/Bucharest` is alphabetically far past that, so it won't appear unless you
**type "Bucharest" in the search box first**. Not a bug in this fork — it's upstream Tactility
behavior, just easy to miss.

## Known upstream state (informational)

This fork tracks upstream Tactility's `main` branch (unreleased, currently labeled `0.8.0-dev`),
not the latest tagged stable release (`v0.7.0`, 2 July 2026). All of this fork's own apps
(BtRemote, ControlCenter, Gateway4G, DeskClock) are built and tested against that dev tree.
Pinning to `v0.7.0` instead would mean porting past 100 commits of upstream changes — including
deep refactors (kernel-owned services, logging macros) — backwards; not attempted, not
recommended without a concrete reason.

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
