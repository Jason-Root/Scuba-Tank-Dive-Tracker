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
