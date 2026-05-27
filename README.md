# Scuba Tank Dive Tracker

Firmware for a Heltec Vision Master E290 / ESP32-S3 e-paper dive display.

The sketch is self-contained: it connects to WiFi, logs in to Subsurface Cloud,
fetches `/api/stats`, optionally reads a public calendar `.ics` feed for the
next dive, updates the e-paper display, then sleeps until the next daily update
or optional reed-switch wake.

## Configure

Edit the `EDIT THESE SETTINGS` section near the top of `DiveInfoV1_3.ino`.
Most users should only need this block:

```cpp
const char* WIFI_NAME = "YOUR_2G_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* SUBSURFACE_USERNAME = "";
const char* SUBSURFACE_PASSWORD = "";

const char* CALENDAR_LINK = "";

const bool USE_REED_SWITCH = true;
const bool USE_AUTO_ROTATION = true;
const int SCREEN_ROTATION = 0;

const int LOW_BATTERY_PERCENT = 10;
```

Set `USE_REED_SWITCH` to `false` for builds without the magnetic reed switch.
Set `USE_AUTO_ROTATION` to `false` for builds without the IMU/9DOF board,
then choose `SCREEN_ROTATION` as `0`, `90`, `180`, or `270`.

The ESP32-S3 only supports 2.4 GHz WiFi.

## Battery

The Vision Master E290 battery sense path uses:

- GPIO7 for ADC voltage
- GPIO46 as `INPUT_PULLUP`
- scale multiplier `4.90`

If the estimated battery is at or below `LOW_BATTERY_PERCENT`, the daily update
shows only a battery symbol and `charge now` instead of dive stats.

## Build And Upload

Open `DiveInfoV1_3.ino` in the Arduino IDE, select the Heltec Vision Master
E290 / ESP32-S3 board support you use for your device, edit the settings block,
then upload over USB.
