#pragma once

// Copy this file to src/secrets.h and fill in your own values.
// src/secrets.h is ignored by Git.

// Wi-Fi credentials
static const char* wifiList[][2] = {
  {"YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"},
};
static const int WIFI_COUNT = sizeof(wifiList) / sizeof(wifiList[0]);

// JSON endpoint serving dive stats
static const char* JSON_URL = "https://your-worker-name.workers.dev/";

// OTA settings
static const char* OTA_HOSTNAME = "diveinfo-e290";
static const char* OTA_PASSWORD = "";

// Internet OTA settings (optional)
// Manifest JSON format:
// {
//   "version": "v1.6.0",
//   "bin_url": "https://github.com/<user>/<repo>/releases/download/v1.6.0/firmware.bin"
// }
#define INTERNET_OTA_MANIFEST_URL ""
#define INTERNET_OTA_MIN_BATTERY_PERCENT 25.0f
#define INTERNET_OTA_CHECK_ON_TIMER_WAKE 1
