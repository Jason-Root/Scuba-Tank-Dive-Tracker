# Scuba Tank Dive Tracker (Vision Master E290)

This project runs on a Heltec Vision Master E290 and displays dive stats on e-paper.

It supports:
- Normal low-power operation with deep sleep
- Orientation-based display layout
- Magnet/reed-switch wake
- OTA firmware updates
- Daily data refresh from a JSON endpoint

## What It Displays

The firmware expects JSON data from:
- `https://diveinfo.jason-aa7.workers.dev/`

Expected fields:
- `totalDives`
- `totalMinutesUnderwater`
- `deepestDive`
- `daysUntil`
- `nextDive`

## Wake + Sleep Behavior

- Device wakes from:
  - Reed switch (`WAKE_PIN` LOW)
  - 24-hour timer wake
- After handling wake logic, it returns to deep sleep.

## Data Refresh Behavior

- Forced refresh (`?refresh=1`) happens only when:
  - The 24-hour timer wake fires
  - OTA mode is entered
- Other wake events show cached values from RTC memory.

## Magnet / Orientation Behavior

- Reed switch LOW opens a short orientation check window.
- OTA mode is entered when rotation matches `OTA_ROTATION` (currently `270` in code trigger logic).
- Side orientations are rendered at 90-degree display orientation to avoid showing a 270-degree-rendered screen.

Relevant constants in `src/DiveInfoV1_3.ino`:
- `OTA_ROTATION`
- `OTA_WINDOW_MS`
- `WAKE_PIN`

## OTA Behavior

- OTA mode screen shows:
  - Logo + battery
  - Current device IP
- OTA listens for `OTA_WINDOW_MS` (currently 60s).
- If no OTA upload occurs, firmware reboots.

## Build and Upload (PlatformIO)

`platformio.ini` is configured with:
- `vision_master_e290` (default): OTA upload (`espota`)
- `vision_master_e290_usb`: emergency USB upload (`esptool`, `COM9`)

Commands:

```powershell
# Default (OTA)
platformio run -e vision_master_e290 -t upload

# Emergency USB
platformio run -e vision_master_e290_usb -t upload
```

## First-Time Configuration Checklist

Edit `src/DiveInfoV1_3.ino`:
- Wi-Fi list in `wifiList`
- `JSON_URL`
- OTA settings:
  - `OTA_HOSTNAME`
  - `OTA_PASSWORD` (set non-empty for protection)
  - `OTA_ROTATION` if you want a different trigger orientation

Edit `platformio.ini`:
- `upload_port` under `env:vision_master_e290` to current OTA IP shown on the device
- `upload_port` under `env:vision_master_e290_usb` if COM port changes

## Notes

- `src/worker.js` is included in this repo as a Cloudflare Worker source for generating the JSON payload.
- OTA upload requires the board to be on the OTA screen and reachable on the same network.
