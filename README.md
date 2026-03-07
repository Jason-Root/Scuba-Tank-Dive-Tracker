# Scuba Tank Dive Tracker (Vision Master E290)

This project runs on a Heltec Vision Master E290 and displays dive stats on e-paper.

It supports:
- Normal low-power operation with deep sleep
- Orientation-based display layout
- Magnet/reed-switch wake
- OTA firmware updates
- Daily data refresh from a JSON endpoint
- Auto firmware version from Git (`git describe --tags --always --dirty`)

## What It Displays

The firmware fetches JSON from `JSON_URL` defined in `src/secrets.h`.

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

Relevant constants in `src/DiveInfo.ino`:
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

Firmware secrets/config:
- Copy `src/secrets.example.h` to `src/secrets.h`
- Set your own:
  - `wifiList`
  - `JSON_URL`
  - `OTA_HOSTNAME`
  - `OTA_PASSWORD` (set non-empty for protection)

Runtime settings in code (`src/DiveInfo.ino`):
- `OTA_ROTATION` if you want a different trigger orientation

Edit `platformio.ini`:
- `upload_port` under `env:vision_master_e290` to current OTA IP shown on the device
- `upload_port` under `env:vision_master_e290_usb` if COM port changes

Worker secrets/config (`src/worker.js`):
- Set Cloudflare Worker env vars:
  - `ICS_URL`
  - `DIVEL0GS_USER`
  - `DIVEL0GS_PASS`

## Notes

- `src/worker.js` is included in this repo as a Cloudflare Worker source for generating the JSON payload.
- OTA upload requires the board to be on the OTA screen and reachable on the same network.
- `src/secrets.h` is intentionally ignored by Git so personal Wi-Fi and endpoint values are not committed.
- Firmware version shown on screen is injected at build time from Git via `scripts/git_version.py`.
