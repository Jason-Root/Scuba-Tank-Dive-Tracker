/*
   Heltec Vision Master E290 / ESP32-S3
   Main Dive Display
   Pulls dive stats directly from Subsurface Cloud
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "HT_DEPG0290BxS800FxX_BW.h"
#include "qrcodegen.h"
#include <Adafruit_LSM6DSOX.h>
#include <driver/rtc_io.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

// ================= EDIT THESE SETTINGS =================
// You should only need to change this block before flashing.

const char* WIFI_NAME = "YOUR_2G_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* SUBSURFACE_USERNAME = "";
const char* SUBSURFACE_PASSWORD = "";

// Optional public .ics calendar link. Leave blank to disable the next-dive line.
const char* CALENDAR_LINK = "";

// Set to false if your build does not have a magnetic reed switch.
const bool USE_REED_SWITCH = true;

// Set to false if your build does not have the 9DOF/IMU rotation sensor.
const bool USE_AUTO_ROTATION = true;
const int SCREEN_ROTATION = 0;  // Used when auto rotation is off: 0, 90, 180, or 270.

// At or below this percent, the display shows only "charge now".
const int LOW_BATTERY_PERCENT = 10;

// ================= INTERNAL SETTINGS =================
const char* FIRMWARE_BUILD = __DATE__ " " __TIME__;
const char* SUBSURFACE_BASE = "https://cloud.subsurface-divelog.org";
const char* LOCAL_TIMEZONE = "EST5EDT,M3.2.0/2,M11.1.0/2";
const char* NTP_SERVER = "pool.ntp.org";
const char* SETUP_AP_NAME = "ScubaTracker-Setup";
const char* SETUP_PORTAL_URL = "http://192.168.4.1/";
const uint8_t MAX_SAVED_WIFI = 3;
const byte DNS_PORT = 53;

const char* WIFI_NETWORKS[][2] = {
  {WIFI_NAME, WIFI_PASSWORD}
};
const int WIFI_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

const int VBAT_READ_PIN = 7;
const int VEXT_CTRL_PIN = 18;
const int BATTERY_CTRL_PIN = 46;
const float BATTERY_ADC_MULTIPLIER = 4.90f;
const uint8_t BATTERY_SAMPLE_COUNT = 16;

const int WAKE_PIN = 17;
const int SDA_PIN = 39;
const int SCL_PIN = 38;
const int DISPLAY_CS_PIN = 5;
const int DISPLAY_DC_PIN = 4;
const int DISPLAY_RST_PIN = 3;
const int DISPLAY_BUSY_PIN = 6;
const int DISPLAY_SCK_PIN = 2;
const int DISPLAY_MOSI_PIN = 1;
const int DISPLAY_POWER_PIN = -1;
const uint32_t DISPLAY_SPI_FREQUENCY = 6000000;

// ================= HARDWARE =================
DEPG0290BxS800FxX_BW display(
  DISPLAY_CS_PIN,
  DISPLAY_DC_PIN,
  DISPLAY_RST_PIN,
  DISPLAY_BUSY_PIN,
  DISPLAY_SCK_PIN,
  DISPLAY_MOSI_PIN,
  DISPLAY_POWER_PIN,
  DISPLAY_SPI_FREQUENCY
);

DNSServer dnsServer;
WebServer setupServer(80);

// ================= ORIENTATION =================
Adafruit_LSM6DSOX sox;
int savedRotation = 0;

// ================= MARGINS =================
const int MARGIN_LEFT  = 8;
const int MARGIN_RIGHT = 10;
const int MARGIN_TOP   = 10;
// helper: make WAKE_PIN stable as an EXT1 wake source
static void setupWakePinRTC(gpio_num_t pin) {
  // Route pin through RTC domain (required for reliable EXT1)
  rtc_gpio_deinit(pin);
  rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);

  // Enable RTC pullup so it stays HIGH in deep sleep when reed is open
  rtc_gpio_pullup_en(pin);
  rtc_gpio_pulldown_dis(pin);

  // Hold the RTC configuration through deep sleep
  rtc_gpio_hold_en(pin);
}

// helper: release hold after waking (optional but good practice)
static void releaseWakePinRTC(gpio_num_t pin) {
  rtc_gpio_hold_dis(pin);
}

static bool confirmedLow(uint32_t ms = 60) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (digitalRead(WAKE_PIN) != LOW) return false;
    delay(2);
  }
  return true;
}
// Fake bold helper
void drawStringBold(int x, int y, const String &text) {
  display.drawString(x, y, text);
  display.drawString(x + 1, y, text);
  display.drawString(x, y + 1, text);
  display.drawString(x + 1, y + 1, text);
}

// Heltec Vision Master E290 peripheral rails.
void VextON() {
  pinMode(VEXT_CTRL_PIN, OUTPUT);
  digitalWrite(VEXT_CTRL_PIN, HIGH);
}

void VextOFF() {
  pinMode(VEXT_CTRL_PIN, OUTPUT);
  digitalWrite(VEXT_CTRL_PIN, LOW);
  pinMode(BATTERY_CTRL_PIN, INPUT_PULLDOWN);
}

void initBatteryMonitor() {
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_READ_PIN, ADC_11db);
  pinMode(BATTERY_CTRL_PIN, INPUT_PULLUP);
  pinMode(VBAT_READ_PIN, INPUT);
  VextON();
}

float readBatteryVoltage() {
  VextON();
  pinMode(BATTERY_CTRL_PIN, INPUT_PULLUP);
  pinMode(VBAT_READ_PIN, INPUT);
  delay(50);

  uint32_t rawSum = 0;
  uint32_t millivoltSum = 0;

  for (uint8_t i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    rawSum += analogRead(VBAT_READ_PIN);
    millivoltSum += analogReadMilliVolts(VBAT_READ_PIN);
    delay(2);
  }

  float rawAverage = rawSum / (float)BATTERY_SAMPLE_COUNT;
  float adcMillivolts = millivoltSum / (float)BATTERY_SAMPLE_COUNT;
  float voltage = (adcMillivolts * BATTERY_ADC_MULTIPLIER) / 1000.0f;

  Serial.printf("Battery ADC raw=%.0f adc=%.0f mV battery=%.2f V\n",
                rawAverage,
                adcMillivolts,
                voltage);

  return voltage;
}

int estimateBatteryPercent(float voltage) {
  float percent;

  if (voltage >= 4.20f) {
    percent = 100.0f;
  } else if (voltage >= 4.00f) {
    percent = 80.0f + ((voltage - 4.00f) / 0.20f) * 20.0f;
  } else if (voltage >= 3.85f) {
    percent = 55.0f + ((voltage - 3.85f) / 0.15f) * 25.0f;
  } else if (voltage >= 3.75f) {
    percent = 35.0f + ((voltage - 3.75f) / 0.10f) * 20.0f;
  } else if (voltage >= 3.65f) {
    percent = 15.0f + ((voltage - 3.65f) / 0.10f) * 20.0f;
  } else if (voltage >= 3.30f) {
    percent = ((voltage - 3.30f) / 0.35f) * 15.0f;
  } else {
    percent = 0.0f;
  }

  return (int)(constrain(percent, 0.0f, 100.0f) + 0.5f);
}

// ================= ORIENTATION FUNCTIONS =================
int normalizeRotation(int rotation) {
  if (rotation == 90 || rotation == 180 || rotation == 270) return rotation;
  return 0;
}

int detectOrientation() {
  if (!USE_AUTO_ROTATION) {
    return normalizeRotation(SCREEN_ROTATION);
  }

  sensors_event_t accel;
  sensors_event_t gyro;
  sensors_event_t temp;
  sox.getEvent(&accel, &gyro, &temp);

  if (abs(accel.acceleration.x) > abs(accel.acceleration.y)) {
    if (accel.acceleration.x > 0) return 90;
    else return 270;
  } else {
    if (accel.acceleration.y > 0) return 0;
    else return 180;
  }
}

void applyRotation(int rotation) {
  switch(rotation) {
    case 0:   display.screenRotate(ANGLE_0_DEGREE); break;
    case 90:  display.screenRotate(ANGLE_270_DEGREE); break;
    case 180: display.screenRotate(ANGLE_180_DEGREE); break;
    case 270: display.screenRotate(ANGLE_90_DEGREE); break;
    default:  display.screenRotate(ANGLE_0_DEGREE); break;
  }
}

// ================= LOW BATTERY SCREEN =================
bool isHorizontalRotation() {
  return savedRotation == 0 || savedRotation == 180;
}

void drawBatterySymbol(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h);
  display.drawRect(x + w, y + h / 4, 5, h / 2);
  display.fillRect(x + 4, y + 4, max(3, w / 8), h - 8);
}

void showChargeNowScreen() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);

  if (isHorizontalRotation()) {
    drawBatterySymbol(100, 24, 88, 38);
    display.setFont(ArialMT_Plain_24);
    drawStringBold(148, 76, "charge now");
  } else {
    drawBatterySymbol(34, 78, 56, 28);
    display.setFont(ArialMT_Plain_24);
    drawStringBold(64, 124, "charge");
    drawStringBold(64, 156, "now");
  }

  display.display();
}

// ================= MAIN DISPLAY =================
void showStats(const String& dives,
               const String& minutes,
               const String& deepest,
               int daysUntil,
               const String& nextDive) {
  display.clear();
  display.setFont(ArialMT_Plain_16);

  const int screenW = 296;
  const int innerW = screenW - MARGIN_LEFT - MARGIN_RIGHT;
  const int centerX = MARGIN_LEFT + (innerW / 2);

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  drawStringBold(MARGIN_LEFT, MARGIN_TOP, dives + " Dives Total");

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  drawStringBold(screenW - MARGIN_RIGHT, MARGIN_TOP,
                 "Deepest Dive: " + deepest + " ");

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  drawStringBold(centerX, MARGIN_TOP + 30,
                 "Spent " + minutes + " Underwater");

  const int gapW = 140;
  const int yLine = MARGIN_TOP + 70;

  display.drawHorizontalLine(MARGIN_LEFT, yLine, centerX - gapW / 2 - MARGIN_LEFT);
  display.drawHorizontalLine(centerX + gapW / 2, yLine,
                             (screenW - MARGIN_RIGHT) - (centerX + gapW / 2));

  drawStringBold(centerX, yLine - 9, "Next dive in:");

  String nextLine = String(daysUntil) + " days  -  " + nextDive;
  if (nextLine.length() > 36) nextLine = nextLine.substring(0,33) + "...";

  drawStringBold(centerX, yLine + 14, nextLine);

  display.display();
}

void showStatsVertical(const String& dives,
                       const String& minutes,
                       const String& deepest,
                       int daysUntil,
                       const String& nextDive)
{
  display.clear();

  const int centerX = 128 / 2;   // vertical screen width
  int y = 35;

  display.setTextAlignment(TEXT_ALIGN_CENTER);

  // ---------- TOTAL DIVES ----------
  display.setFont(ArialMT_Plain_16);
  display.drawString(centerX, y, "Total Dives");
  y += 20;

  display.setFont(ArialMT_Plain_24);
  drawStringBold(centerX, y, dives);
  y += 36;

  // ---------- DEEPEST DIVE ----------
  display.setFont(ArialMT_Plain_16);
  display.drawString(centerX, y, "Deepest Dive");
  y += 20;

  display.setFont(ArialMT_Plain_24);
  drawStringBold(centerX, y, deepest);
  y += 36;

  // ---------- UNDERWATER ----------
  display.setFont(ArialMT_Plain_16);
  display.drawString(centerX, y, "Time Underwater");
  y += 20;

  display.setFont(ArialMT_Plain_24);
  drawStringBold(centerX, y, minutes);
  y += 36;

  // ---------- NEXT DIVE ----------
  display.setFont(ArialMT_Plain_16);
  display.drawString(centerX, y, "Next Dive In");
  y += 20;

  display.setFont(ArialMT_Plain_24);
  drawStringBold(centerX, y, String(daysUntil) + " days");

  display.display();
}

void showUpdateError(const String& message) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);

  if (isHorizontalRotation()) {
    display.setFont(ArialMT_Plain_24);
    drawStringBold(148, 34, "Update Failed");
    display.setFont(ArialMT_Plain_10);
    display.drawString(148, 76, message.substring(0, 38));
  } else {
    display.setFont(ArialMT_Plain_24);
    drawStringBold(64, 78, "Update");
    drawStringBold(64, 110, "Failed");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 154, message.substring(0, 18));
  }

  display.display();
}

// ================= WIFI DEBUG =================
const uint8_t WIFI_DEBUG_MAX_LINES = 10;
String wifiDebugLines[WIFI_DEBUG_MAX_LINES];
uint8_t wifiDebugLineCount = 0;

void addWiFiDebugLine(const String& line) {
  if (wifiDebugLineCount < WIFI_DEBUG_MAX_LINES) {
    wifiDebugLines[wifiDebugLineCount++] = line;
  }
}

String wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no ssid";
    case WL_SCAN_COMPLETED: return "scan done";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "bad pass/auth";
    case WL_CONNECTION_LOST: return "lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "status " + String((int)status);
  }
}

struct WiFiCredential {
  String ssid;
  String password;
};

bool isUsableWiFiCredential(const String& ssid) {
  return ssid.length() > 0 && ssid != "YOUR_2G_WIFI_NAME";
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

uint8_t loadSavedWiFiCredentials(WiFiCredential saved[]) {
  Preferences preferences;
  uint8_t count = 0;

  if (!preferences.begin("wifi-list", true)) return 0;

  for (uint8_t i = 0; i < MAX_SAVED_WIFI; i++) {
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);
    String ssid = preferences.getString(ssidKey.c_str(), "");
    if (isUsableWiFiCredential(ssid)) {
      saved[count].ssid = ssid;
      saved[count].password = preferences.getString(passKey.c_str(), "");
      count++;
    }
  }

  preferences.end();
  return count;
}

void saveWiFiCredential(const String& ssid, const String& password) {
  WiFiCredential saved[MAX_SAVED_WIFI];
  uint8_t existingCount = loadSavedWiFiCredentials(saved);

  Preferences preferences;
  if (!preferences.begin("wifi-list", false)) return;

  preferences.putString("ssid0", ssid);
  preferences.putString("pass0", password);

  uint8_t writeIndex = 1;
  for (uint8_t i = 0; i < existingCount && writeIndex < MAX_SAVED_WIFI; i++) {
    if (saved[i].ssid == ssid) continue;
    String ssidKey = "ssid" + String(writeIndex);
    String passKey = "pass" + String(writeIndex);
    preferences.putString(ssidKey.c_str(), saved[i].ssid);
    preferences.putString(passKey.c_str(), saved[i].password);
    writeIndex++;
  }

  for (uint8_t i = writeIndex; i < MAX_SAVED_WIFI; i++) {
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);
    preferences.remove(ssidKey.c_str());
    preferences.remove(passKey.c_str());
  }

  preferences.end();
}

void drawQRCode(const String& payload, int x, int y, int maxPixels) {
  uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];

  bool ok = qrcodegen_encodeText(payload.c_str(),
                                 temp,
                                 qrcode,
                                 qrcodegen_Ecc_LOW,
                                 qrcodegen_VERSION_MIN,
                                 6,
                                 qrcodegen_Mask_AUTO,
                                 true);
  if (!ok) return;

  int size = qrcodegen_getSize(qrcode);
  int scale = max(1, maxPixels / (size + 8));
  int quiet = 4 * scale;
  int qrPixels = size * scale;

  display.drawRect(x, y, qrPixels + quiet * 2, qrPixels + quiet * 2);

  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      if (qrcodegen_getModule(qrcode, col, row)) {
        display.fillRect(x + quiet + col * scale,
                         y + quiet + row * scale,
                         scale,
                         scale);
      }
    }
  }
}

void showWiFiSetupScreen(IPAddress apIP) {
  display.clear();
  String qrPayload = "WIFI:T:nopass;S:" + String(SETUP_AP_NAME) + ";;";

  if (isHorizontalRotation()) {
    drawQRCode(qrPayload, 8, 8, 112);

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_16);
    drawStringBold(132, 10, "WiFi setup");

    display.setFont(ArialMT_Plain_10);
    display.drawString(132, 38, "Scan QR to join:");
    display.drawString(132, 52, SETUP_AP_NAME);
    display.drawString(132, 74, "Then open:");
    display.drawString(132, 88, String("http://") + apIP.toString());
  } else {
    drawQRCode(qrPayload, 14, 8, 100);

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    drawStringBold(64, 126, "WiFi setup");

    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 152, "Scan QR to join");
    display.drawString(64, 168, SETUP_AP_NAME);
    display.drawString(64, 194, String("Open ") + apIP.toString());
  }

  display.display();
}

String buildWiFiSetupPage(const String& message = "") {
  int networks = WiFi.scanNetworks(false, true);

  String html = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Scuba Tracker WiFi</title><style>";
  html += "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f6f7f9;color:#111}";
  html += "main{max-width:520px;margin:0 auto;padding:24px}h1{font-size:24px;margin:0 0 8px}";
  html += "p{line-height:1.4}label{display:block;font-weight:650;margin:18px 0 8px}";
  html += "select,input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;border-radius:8px;border:1px solid #bbb;background:white}";
  html += "button{margin-top:22px;background:#111;color:white;border:0;font-weight:700}";
  html += ".msg{padding:12px;border-radius:8px;background:#e6f4ea;margin:16px 0}.hint{color:#555;font-size:14px}";
  html += "</style></head><body><main><h1>Scuba Tracker WiFi</h1>";
  html += "<p>Select a 2.4 GHz WiFi network and save it. The tracker will restart and use it for future updates.</p>";

  if (message.length() > 0) {
    html += "<div class='msg'>" + htmlEscape(message) + "</div>";
  }

  html += "<form method='post' action='/save'>";
  html += "<label for='ssid'>WiFi network</label><select id='ssid' name='ssid'>";

  if (networks <= 0) {
    html += "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < networks; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      html += "<option value='" + htmlEscape(ssid) + "'>";
      html += htmlEscape(ssid) + " (" + String(WiFi.RSSI(i)) + " dBm";
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) html += ", open";
      html += ")</option>";
    }
  }

  html += "<option value='__manual__'>Type network name manually</option>";
  html += "</select>";
  html += "<label for='manual_ssid'>Manual network name</label>";
  html += "<input id='manual_ssid' name='manual_ssid' placeholder='Only needed if hidden'>";
  html += "<label for='password'>WiFi password</label>";
  html += "<input id='password' name='password' type='password' autocomplete='current-password'>";
  html += "<button type='submit'>Save WiFi</button></form>";
  html += "<p class='hint'><a href='/'>Scan again</a></p>";
  html += "</main></body></html>";

  WiFi.scanDelete();
  return html;
}

void handleSetupRoot() {
  setupServer.send(200, "text/html", buildWiFiSetupPage());
}

void handleSetupSave() {
  String ssid = setupServer.arg("ssid");
  String password = setupServer.arg("password");

  if (ssid == "__manual__") ssid = setupServer.arg("manual_ssid");
  ssid.trim();

  if (!isUsableWiFiCredential(ssid)) {
    setupServer.send(200, "text/html", buildWiFiSetupPage("Enter a WiFi network name first."));
    return;
  }

  saveWiFiCredential(ssid, password);
  setupServer.send(200, "text/html",
                   "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<title>Saved</title></head><body style='font-family:system-ui;padding:24px'>"
                   "<h1>WiFi saved</h1><p>The tracker is restarting now.</p></body></html>");

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_24);
  drawStringBold(isHorizontalRotation() ? 148 : 64,
                 isHorizontalRotation() ? 44 : 118,
                 "WiFi saved");
  display.setFont(ArialMT_Plain_10);
  display.drawString(isHorizontalRotation() ? 148 : 64,
                     isHorizontalRotation() ? 82 : 154,
                     "Restarting...");
  display.display();

  delay(1500);
  ESP.restart();
}

void handleSetupNotFound() {
  setupServer.sendHeader("Location", SETUP_PORTAL_URL, true);
  setupServer.send(302, "text/plain", "");
}

void startWiFiSetupPortal() {
  Serial.println("Starting WiFi setup portal");
  WiFi.disconnect(false, true);
  delay(250);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(SETUP_AP_NAME);

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIP);

  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.on("/generate_204", HTTP_GET, handleSetupRoot);
  setupServer.on("/hotspot-detect.html", HTTP_GET, handleSetupRoot);
  setupServer.on("/fwlink", HTTP_GET, handleSetupRoot);
  setupServer.onNotFound(handleSetupNotFound);
  setupServer.begin();

  showWiFiSetupScreen(apIP);

  while (true) {
    dnsServer.processNextRequest();
    setupServer.handleClient();
    delay(2);
  }
}

// ================= DIVE DATA =================
struct DiveInfo {
  bool ok = false;
  String error;
  String totalDives = "0";
  String totalMinutesUnderwater = "0 min";
  String deepestDive = "0 ft";
  String nextDive = "No dive trips planned";
  int daysUntil = 0;
};

String formatMinutes(int minutes) {
  int min = max(0, minutes);
  if (min < 60) return String(min) + " min";

  int h = min / 60;
  int m = min % 60;
  return m == 0 ? String(h) + " h" : String(h) + " h " + String(m) + " min";
}

int metersToFeet(float meters) {
  return (int)(meters * 3.28084f + 0.5f);
}

String urlEncode(const String& value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;

  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

String findCsrfToken(const String& html) {
  int nameAt = html.indexOf("name=\"csrf_token\"");
  if (nameAt < 0) nameAt = html.indexOf("name='csrf_token'");
  if (nameAt < 0) return "";

  int tagStart = html.lastIndexOf('<', nameAt);
  int tagEnd = html.indexOf('>', nameAt);
  if (tagStart < 0 || tagEnd < 0) return "";

  String tag = html.substring(tagStart, tagEnd + 1);
  int valueAt = tag.indexOf("value=\"");
  char quote = '"';
  if (valueAt < 0) {
    valueAt = tag.indexOf("value='");
    quote = '\'';
  }
  if (valueAt < 0) return "";

  valueAt += 7;
  int valueEnd = tag.indexOf(quote, valueAt);
  if (valueEnd < 0) return "";
  return tag.substring(valueAt, valueEnd);
}

const char* SUBSURFACE_HEADER_KEYS[] = {"Set-Cookie"};
const size_t SUBSURFACE_HEADER_COUNT = sizeof(SUBSURFACE_HEADER_KEYS) / sizeof(SUBSURFACE_HEADER_KEYS[0]);

String cookiePairFromSetCookie(const String& setCookie) {
  String pair = setCookie;
  int semi = pair.indexOf(';');
  if (semi >= 0) pair = pair.substring(0, semi);
  pair.trim();

  int equals = pair.indexOf('=');
  if (equals <= 0) return "";
  return pair;
}

void updateCookieHeader(String& cookieHeader, const String& setCookie) {
  String pair = cookiePairFromSetCookie(setCookie);
  if (pair.length() == 0) return;

  int equals = pair.indexOf('=');
  String name = pair.substring(0, equals);

  int start = 0;
  while (start < (int)cookieHeader.length()) {
    int end = cookieHeader.indexOf("; ", start);
    if (end < 0) end = cookieHeader.length();

    String existing = cookieHeader.substring(start, end);
    int existingEquals = existing.indexOf('=');
    if (existingEquals > 0 && existing.substring(0, existingEquals) == name) {
      String before = start > 0 ? cookieHeader.substring(0, start) : "";
      String after = end < (int)cookieHeader.length() ? cookieHeader.substring(end + 2) : "";

      cookieHeader = before;
      if (cookieHeader.length() > 0) cookieHeader += "; ";
      cookieHeader += pair;
      if (after.length() > 0) {
        cookieHeader += "; ";
        cookieHeader += after;
      }
      return;
    }

    start = end + 2;
  }

  if (cookieHeader.length() > 0) cookieHeader += "; ";
  cookieHeader += pair;
}

void addCookieHeader(HTTPClient& http, const String& cookieHeader) {
  if (cookieHeader.length() > 0) {
    http.addHeader("Cookie", cookieHeader);
  }
}

bool syncClock() {
  setenv("TZ", LOCAL_TIMEZONE, 1);
  tzset();
  configTime(0, 0, NTP_SERVER);

  for (uint8_t i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) return true;
    delay(500);
  }

  return false;
}

long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

long todayDays() {
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  return daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

String nextUnfoldedIcsLine(const String& text, int& pos) {
  String line;

  while (pos < (int)text.length()) {
    int end = text.indexOf('\n', pos);
    if (end < 0) end = text.length();

    String part = text.substring(pos, end);
    part.trim();
    pos = end + 1;

    if (part.length() == 0) continue;
    line = part;
    break;
  }

  while (pos < (int)text.length()) {
    int end = text.indexOf('\n', pos);
    if (end < 0) end = text.length();

    String peek = text.substring(pos, end);
    if (!(peek.startsWith(" ") || peek.startsWith("\t"))) break;

    peek.remove(0, 1);
    peek.trim();
    line += peek;
    pos = end + 1;
  }

  return line;
}

String cleanIcsText(String value) {
  value.replace("\\,", ",");
  value.replace("\\;", ";");
  value.replace("\\n", " ");
  value.replace("\\N", " ");
  return value;
}

bool fetchText(const char* url, String& body, String& error) {
  String target(url);

  if (target.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, target)) {
      error = "HTTP begin failed";
      return false;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();

    if (code <= 0) {
      error = HTTPClient::errorToString(code);
      http.end();
      return false;
    }

    body = http.getString();
    http.end();

    if (code < 200 || code >= 300) {
      error = "HTTP " + String(code);
      return false;
    }

    return true;
  }

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, target)) {
    error = "HTTP begin failed";
    return false;
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();

  if (code <= 0) {
    error = HTTPClient::errorToString(code);
    http.end();
    return false;
  }

  body = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    error = "HTTP " + String(code);
    return false;
  }

  return true;
}

void parseNextDive(const String& icsText, DiveInfo& info) {
  String summary;
  long eventDay = -1;
  long bestDay = LONG_MAX;
  String bestSummary;
  bool inEvent = false;
  int pos = 0;
  long today = todayDays();

  while (pos < (int)icsText.length()) {
    String line = nextUnfoldedIcsLine(icsText, pos);
    if (line.length() == 0) continue;

    if (line == "BEGIN:VEVENT") {
      inEvent = true;
      summary = "";
      eventDay = -1;
      continue;
    }

    if (line == "END:VEVENT") {
      if (inEvent && eventDay >= today && eventDay < bestDay) {
        bestDay = eventDay;
        bestSummary = summary.length() ? summary : "Upcoming dive";
      }
      inEvent = false;
      continue;
    }

    if (!inEvent) continue;

    if (line.startsWith("SUMMARY")) {
      int colon = line.indexOf(':');
      if (colon >= 0) summary = cleanIcsText(line.substring(colon + 1));
    } else if (line.startsWith("DTSTART")) {
      int colon = line.indexOf(':');
      if (colon < 0 || colon + 8 >= (int)line.length()) continue;

      String value = line.substring(colon + 1);
      int year = value.substring(0, 4).toInt();
      int month = value.substring(4, 6).toInt();
      int day = value.substring(6, 8).toInt();
      if (year > 2000 && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        eventDay = daysFromCivil(year, month, day);
      }
    }
  }

  if (bestDay != LONG_MAX) {
    info.nextDive = bestSummary;
    info.daysUntil = max(0L, bestDay - today);
  }
}

bool fetchSubsurfaceStats(DiveInfo& info) {
  if (String(SUBSURFACE_USERNAME).length() == 0 || String(SUBSURFACE_PASSWORD).length() == 0) {
    info.error = "Set Subsurface login";
    return false;
  }

  String sessionCookies;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  String loginPageUrl = String(SUBSURFACE_BASE) + "/login?next=/";
  if (!http.begin(client, loginPageUrl)) {
    info.error = "Subsurface begin failed";
    return false;
  }

  http.addHeader("Accept", "text/html");
  http.collectHeaders(SUBSURFACE_HEADER_KEYS, SUBSURFACE_HEADER_COUNT);
  int code = http.GET();
  String loginPage = http.getString();
  updateCookieHeader(sessionCookies, http.header("Set-Cookie"));
  Serial.println("Login page HTTP " + String(code) + ", cookie bytes " + String(sessionCookies.length()));
  http.end();

  if (code < 200 || code >= 300) {
    info.error = "Login page HTTP " + String(code);
    return false;
  }

  String csrfToken = findCsrfToken(loginPage);
  if (csrfToken.length() == 0) {
    info.error = "No CSRF token";
    return false;
  }
  if (sessionCookies.length() == 0) {
    info.error = "No login cookie";
    return false;
  }

  String loginBody = "username=" + urlEncode(SUBSURFACE_USERNAME) +
                     "&password=" + urlEncode(SUBSURFACE_PASSWORD) +
                     "&csrf_token=" + urlEncode(csrfToken);

  WiFiClientSecure loginClient;
  loginClient.setInsecure();
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  if (!http.begin(loginClient, String(SUBSURFACE_BASE) + "/login")) {
    info.error = "Login begin failed";
    return false;
  }

  http.addHeader("Accept", "text/html,application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Referer", loginPageUrl);
  addCookieHeader(http, sessionCookies);
  http.collectHeaders(SUBSURFACE_HEADER_KEYS, SUBSURFACE_HEADER_COUNT);
  code = http.POST(loginBody);
  updateCookieHeader(sessionCookies, http.header("Set-Cookie"));
  Serial.println("Login POST HTTP " + String(code) + ", cookie bytes " + String(sessionCookies.length()));
  http.end();

  if (code < 200 || code >= 400) {
    info.error = "Login HTTP " + String(code);
    return false;
  }
  if (sessionCookies.length() == 0) {
    info.error = "No auth cookie";
    return false;
  }

  WiFiClientSecure statsClient;
  statsClient.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(statsClient, String(SUBSURFACE_BASE) + "/api/stats")) {
    info.error = "Stats begin failed";
    return false;
  }

  http.addHeader("Accept", "application/json");
  addCookieHeader(http, sessionCookies);
  code = http.GET();
  String statsBody = http.getString();
  Serial.println("Stats HTTP " + String(code));
  http.end();

  if (code < 200 || code >= 300) {
    info.error = "Stats HTTP " + String(code);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, statsBody);
  if (err) {
    info.error = "Stats JSON failed";
    return false;
  }

  const char* status = doc["status"] | "success";
  if (String(status) != "success") {
    info.error = "Stats status failed";
    return false;
  }

  JsonVariant statistics = doc["statistics"];
  int totalDives = doc["total_dives"] | 0;
  int totalMinutes = statistics["total_dive_time_minutes"] | 0;
  float maxDepthMeters = statistics["max_depth_meters"] | 0.0f;

  info.totalDives = String(totalDives);
  info.totalMinutesUnderwater = formatMinutes(totalMinutes);
  info.deepestDive = String(metersToFeet(maxDepthMeters)) + " ft";
  return true;
}

bool buildDiveInfo(DiveInfo& info) {
  if (!fetchSubsurfaceStats(info)) return false;

  String icsText;
  String calendarError;
  if (String(CALENDAR_LINK).length() > 0 &&
      fetchText(CALENDAR_LINK, icsText, calendarError)) {
    parseNextDive(icsText, info);
  }

  info.ok = true;
  return true;
}




// ================= WIFI =================
bool tryConnectToWiFi(const String& ssid, const String& password) {
  if (!isUsableWiFiCredential(ssid)) return false;

  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.disconnect(false, false);
  delay(250);
  WiFi.begin(ssid.c_str(), password.c_str());

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    tries++;
  }

  wl_status_t status = WiFi.status();
  addWiFiDebugLine(ssid + " -> " + wifiStatusText(status));
  Serial.print("WiFi status: ");
  Serial.println(wifiStatusText(status));

  return status == WL_CONNECTED;
}

void addNetworkScanDebugLine(const String& ssid, int networks) {
  if (!isUsableWiFiCredential(ssid)) return;

  int bestRssi = -999;
  for (int n = 0; n < networks; n++) {
    if (WiFi.SSID(n) == ssid && WiFi.RSSI(n) > bestRssi) {
      bestRssi = WiFi.RSSI(n);
    }
  }

  if (bestRssi > -999) {
    addWiFiDebugLine(ssid + " seen " + String(bestRssi) + "dBm");
  } else {
    addWiFiDebugLine(ssid + " not seen");
  }
}

bool connectWiFi() {
  wifiDebugLineCount = 0;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(500);
  WiFi.mode(WIFI_STA);

  addWiFiDebugLine("Scanning 2.4GHz...");
  int networks = WiFi.scanNetworks(false, true);
  addWiFiDebugLine("Found " + String(networks) + " networks");
  if (networks < 0) {
    addWiFiDebugLine("Scan error " + String(networks));
  }

  WiFiCredential saved[MAX_SAVED_WIFI];
  uint8_t savedCount = loadSavedWiFiCredentials(saved);

  for (uint8_t i = 0; i < savedCount; i++) {
    addNetworkScanDebugLine(saved[i].ssid, networks);
  }

  for (int i = 0; i < WIFI_COUNT; i++) {
    addNetworkScanDebugLine(WIFI_NETWORKS[i][0], networks);
  }

  WiFi.scanDelete();

  for (uint8_t i = 0; i < savedCount; i++) {
    if (tryConnectToWiFi(saved[i].ssid, saved[i].password)) return true;
  }

  for (int i = 0; i < WIFI_COUNT; i++) {
    if (tryConnectToWiFi(WIFI_NETWORKS[i][0], WIFI_NETWORKS[i][1])) return true;
  }

  return false;
}

// ================= SLEEP =================
void enterDeepSleep() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  VextOFF();

  if (USE_REED_SWITCH) {
    setupWakePinRTC((gpio_num_t)WAKE_PIN);
    esp_sleep_enable_ext1_wakeup(1ULL << WAKE_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
  }

  esp_sleep_enable_timer_wakeup(24ULL * 60ULL * 60ULL * 1000000ULL);

  Serial.print("Going to sleep. Reed=");
  Serial.print(USE_REED_SWITCH ? "on" : "off");
  if (USE_REED_SWITCH) {
    Serial.print(" WAKE_PIN=");
    Serial.print(WAKE_PIN);
    Serial.print(" state=");
    Serial.print(digitalRead(WAKE_PIN));
  }
  Serial.println();

  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(200);

  if (USE_REED_SWITCH) {
    releaseWakePinRTC((gpio_num_t)WAKE_PIN);
    pinMode(WAKE_PIN, INPUT_PULLUP);
  }

  VextON();
  delay(50);

  display.init();
  display.screenRotate(ANGLE_0_DEGREE);
  initBatteryMonitor();

  savedRotation = normalizeRotation(SCREEN_ROTATION);
  if (USE_AUTO_ROTATION) {
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(20);

    if (sox.begin_I2C()) {
      savedRotation = detectOrientation();
    } else {
      Serial.println("IMU not found; using fixed orientation.");
    }
  }
  applyRotation(savedRotation);

  float voltage = readBatteryVoltage();
  int batteryPercent = estimateBatteryPercent(voltage);
  if (batteryPercent <= LOW_BATTERY_PERCENT) {
    Serial.println("Battery low -> charge now screen");
    showChargeNowScreen();
    delay(2000);
    enterDeepSleep();
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.print("Wake cause: ");
  Serial.println((int)cause);

  if (USE_REED_SWITCH && digitalRead(WAKE_PIN) == LOW && confirmedLow()) {
    Serial.println("Reed LOW -> waiting for release");
    Serial.println("Waiting for reed release...");
    while (digitalRead(WAKE_PIN) == LOW) delay(10);
    delay(80); // debounce settle
  }

  if (!connectWiFi()) {
    startWiFiSetupPortal();
  } else {
    DiveInfo info;
    if (!syncClock()) {
      showUpdateError("Time sync failed");
    } else if (!buildDiveInfo(info)) {
      showUpdateError(info.error);
    } else if (isHorizontalRotation()) {
      showStats(
        info.totalDives,
        info.totalMinutesUnderwater,
        info.deepestDive,
        info.daysUntil,
        info.nextDive
      );
    } else {
      showStatsVertical(
        info.totalDives,
        info.totalMinutesUnderwater,
        info.deepestDive,
        info.daysUntil,
        info.nextDive
      );
    }
  }

  delay(2000);
  enterDeepSleep();
}

void loop() {}
