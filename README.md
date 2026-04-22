# Scuba Tank Dive Tracker (Vision Master E290)

This project runs on a Heltec Vision Master E290 and displays dive stats on e-paper.

It supports:
- Normal low-power operation with deep sleep
- Orientation-based display layout
- Magnet/reed-switch wake
- OTA firmware updates
- Optional internet auto-update from GitHub release binaries
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
- OTA mode is entered when the device is held upside down during the reed-switch window.
- Orientation is sampled a few times before deciding, which makes sideways rotation snappier and OTA entry more deliberate.
- Side orientations are rendered at 90-degree display orientation to avoid showing a 270-degree-rendered screen.

Relevant constants in `src/DiveInfo.ino`:
- `OTA_TRIGGER_ROTATION`
- `OTA_TRIGGER_WINDOW_MS`
- `OTA_WINDOW_MS`
- `WAKE_PIN`

## OTA Behavior

- OTA mode screen shows:
  - Logo + battery
  - Current device IP
- OTA listens for `OTA_WINDOW_MS` (currently 60s).
- If no OTA upload occurs, firmware reboots.

## Internet OTA (Optional)

- If `INTERNET_OTA_MANIFEST_URL` is set in `src/secrets.h`, firmware checks for updates:
  - During daily timer wake (`INTERNET_OTA_CHECK_ON_TIMER_WAKE = 1`)
  - When OTA mode is entered with the magnet
- If manifest `version` differs from current firmware version, the device downloads and applies `bin_url`.
- Updates are skipped when battery percent is below `INTERNET_OTA_MIN_BATTERY_PERCENT`.

Manifest JSON example:

```json
{
  "version": "v1.6.0",
  "bin_url": "https://github.com/<user>/<repo>/releases/download/v1.6.0/firmware.bin"
}
```

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
  - `INTERNET_OTA_MANIFEST_URL` (optional)
  - `INTERNET_OTA_MIN_BATTERY_PERCENT` (optional)
  - `INTERNET_OTA_CHECK_ON_TIMER_WAKE` (optional)

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
