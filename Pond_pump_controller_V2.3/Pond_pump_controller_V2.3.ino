/*
  Pond Pump Controller
  ESP32-2432S028 "Cheap yellow display"

  Compiled in Arduino IDE 2.3.6

  A.G.Doswell Aug 2025
  --------------------------------------
  Version: 2.3
  Features:
   - NTP time (UTC, with UK DST adjustment)
   - Sun arc & sun elevation (day)
   - Central crescent moon & stars (night)
   - Local temperature via WeeWX
   - Frost warning banner (<3 °C disables pumps)
   - Pump modes: ON / OFF / AUTO / AUTO+1h
   - Mode toggle via TFT touch (raw X) OR local web interface
   - Pump modes saved/restored in NVS
   - Display: 320x240, rotation=3 (landscape, USB on left)
   - Relays: GPIO22 (Pump1), GPIO27 (Pump2)
   - Buzzer: GPIO26
   - Backlight: GPIO21
   - Web UI: status + pump mode control
   - Temp failure detection (TFT + Web warning)
*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// --- FS/WebServer include shim for ESP32 core 3.3.0 ---
#include <FS.h>
using FS = fs::FS;
using File = fs::File;
#include <WebServer.h>
// ------------------------------------------------------

#include <Preferences.h>
#include <time.h>
#include <math.h>

// ================== WiFi ==================
const char* ssid = "SSID";
const char* password = "PASSWORD";

// ================== Display ==================
TFT_eSPI tft = TFT_eSPI();

// Pins
const int BL_PIN = 21;
const int RELAY1 = 22;
const int RELAY2 = 27;
const int BUZZER_PIN = 26;

// UI colors
#define DAY_BG TFT_BLUE
#define NIGHT_BG TFT_BLACK

// Screen size
const int SCREEN_W = 320;
const int SCREEN_H = 240;

// Sun arc geometry
const int centerX = 160;
const int centerY = 150;
const int radius = 100;

// Track previously drawn sun/moon
int prevSunX = -1;
int prevSunY = -1;

// ================== Time ==================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// UK location
float latitude = 51.8875;
float longitude = -2.0436;

// Sunrise/sunset
int sunriseMinutesUTC = 360;
int sunsetMinutesUTC = 1080;
int lastDay = -1, lastMonth = -1, lastYear = -1;

// ================== Temperature ==================
float currentTemperature = -100.0;
unsigned long lastTempUpdate = 0;
const char* tempURL = "http://weewx.local/weewx/";
const unsigned long tempInterval = 600000UL;  // 10 min
bool tempValid = true;                        

// ================== Pump Modes ==================
enum PumpMode { MODE_ON = 1,
                MODE_OFF = 2,
                MODE_AUTO = 3,
                MODE_AUTO_EXTEND = 4 };
PumpMode pumpMode[2] = { MODE_AUTO, MODE_AUTO };

const char* modeName(PumpMode m) {
  switch (m) {
    case MODE_ON: return "ON";
    case MODE_OFF: return "OFF";
    case MODE_AUTO: return "AUTO";
    case MODE_AUTO_EXTEND: return "AUTO+1h";
  }
  return "?";
}

// Pump rects
const int rectWidth = 120;
const int rectHeight = 30;
const int gap = 20;
const int margin = (SCREEN_W - 2 * rectWidth - gap) / 2;
const int rectY = 200;
const int rect1X = margin;
const int rect2X = margin + rectWidth + gap;

// Touch (XPT2046)
#define XPT_IRQ 36
#define XPT_MOSI 32
#define XPT_MISO 39
#define XPT_SCLK 25
#define XPT_CS 33
SPIClass spiTouch(HSPI);
XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);

unsigned long lastTouchMs = 0;
const unsigned long touchDebounceMs = 300;

// Preferences
Preferences prefs;

// Stars
#define STAR_COUNT 30
int starX[STAR_COUNT], starY[STAR_COUNT];
bool starsDrawn = false;

// Web server
WebServer server(80);

// ================== Prototypes ==================
bool isUK_DST(int d, int m, int y);
void calculateSunTimes(int d, int m, int y);
void drawSunArc();
void updateSunDisplay(float sunElevation, int sr, int ss, int nowM, int nowS, bool isDay);
void fetchTemperature();
void drawPumpRects(bool p1, bool p2);
void drawDegreeSymbol(int x, int y, uint16_t c);
void drawStars();
void refreshPumpOutputs();
void beep(int f, int d);

// ================== Save/Load ==================
void savePumpModes() {
  prefs.putUChar("mode1", (uint8_t)pumpMode[0]);
  prefs.putUChar("mode2", (uint8_t)pumpMode[1]);
}
void loadPumpModes() {
  uint8_t m1 = prefs.getUChar("mode1", (uint8_t)MODE_AUTO);
  uint8_t m2 = prefs.getUChar("mode2", (uint8_t)MODE_AUTO);
  if (m1 < MODE_ON || m1 > MODE_AUTO_EXTEND) m1 = MODE_AUTO;
  if (m2 < MODE_ON || m2 > MODE_AUTO_EXTEND) m2 = MODE_AUTO;
  pumpMode[0] = (PumpMode)m1;
  pumpMode[1] = (PumpMode)m2;
}

// Helpers
void beep(int f, int d) {
  tone(BUZZER_PIN, f, d);
  delay(d);
  noTone(BUZZER_PIN);
}
void cycleMode(int i) {
  PumpMode m = pumpMode[i];
  switch (m) {
    case MODE_ON: m = MODE_OFF; break;
    case MODE_OFF: m = MODE_AUTO; break;
    case MODE_AUTO: m = MODE_AUTO_EXTEND; break;
    case MODE_AUTO_EXTEND: m = MODE_ON; break;
  }
  pumpMode[i] = m;
  savePumpModes();
}
bool shouldPumpBeOn(PumpMode m, bool day, int now, int ss, float t) {
  if (!(t >= 3.0)) return false;
  switch (m) {
    case MODE_ON: return true;
    case MODE_OFF: return false;
    case MODE_AUTO: return day;
    case MODE_AUTO_EXTEND: return (day || (now < ss + 60));
  }
  return false;
}

// Web UI
String statusColor(bool on) {
  return on ? "#90EE90" : "#FFB6B6";
}
String htmlEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

void handleRoot() {
  unsigned long e = timeClient.getEpochTime();
  int min = (e / 60) % 60, h = (e / 3600) % 24;
  int now = h * 60 + min;
  bool day = now >= sunriseMinutesUTC && now < sunsetMinutesUTC;
  bool p1 = shouldPumpBeOn(pumpMode[0], day, now, sunsetMinutesUTC, currentTemperature);
  bool p2 = shouldPumpBeOn(pumpMode[1], day, now, sunsetMinutesUTC, currentTemperature);
  int hourLocal = h;
  unsigned long days = e / 86400;
  int year = 1970;
  while (true) {
    int diy = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    if (days >= (unsigned)diy) {
      days -= diy;
      year++;
    } else break;
  }
  int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) dim[1] = 29;
  int month = 1;
  int dleft = days;
  for (int i = 0; i < 12; i++) {
    if (dleft >= dim[i]) {
      dleft -= dim[i];
      month++;
    } else break;
  }
  int dayNum = 1 + dleft;
  if (isUK_DST(dayNum, month, year)) {
    hourLocal++;
    if (hourLocal >= 24) hourLocal -= 24;
  }
  int sRiseH = sunriseMinutesUTC / 60, sRiseM = sunriseMinutesUTC % 60;
  int sSetH = sunsetMinutesUTC / 60, sSetM = sunsetMinutesUTC % 60;
  if (isUK_DST(dayNum, month, year)) {
    sRiseH = (sRiseH + 1) % 24;
    sSetH = (sSetH + 1) % 24;
  }
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Pond Controller</title><style>"
                "body{font-family:Arial;text-align:center;background:#f0f0f0;padding:16px}"
                ".row{display:flex;justify-content:center;gap:20px;flex-wrap:wrap}"
                ".pump{width:160px;height:90px;line-height:1.2;color:black;font-weight:bold;font-size:18px;"
                "border-radius:12px;text-decoration:none;padding-top:14px;box-shadow:0 2px 6px rgba(0,0,0,0.15)}"
                ".sub{font-size:14px;font-weight:600;opacity:0.9}.meta{margin:10px;color:#222}</style></head><body>";
  char header[96];
  snprintf(header, sizeof(header), "%02d/%02d/%04d %02d:%02d  |  Temp: %s  |  Sunrise %02d:%02d  Sunset %02d:%02d",
           dayNum, month, year, hourLocal, min, (tempValid ? String(currentTemperature, 1).c_str() : "ERR"), sRiseH, sRiseM, sSetH, sSetM);
  html += "<div class='meta'>" + htmlEscape(header) + "</div>";
  if (!tempValid) html += "<div style='background:#FF6666;color:white;padding:8px;font-weight:bold;margin:10px auto;width:80%;'>Temperature source unavailable</div>";
  html += "<h2>Pond Pump Controller</h2><div class='row'>";
  html += "<a class='pump' style='background:" + statusColor(p1) + "' href='/pump1'>Pump 1<br><span class='sub'>" + String(p1 ? "ON" : "OFF") + " • " + modeName(pumpMode[0]) + "</span></a>";
  html += "<a class='pump' style='background:" + statusColor(p2) + "' href='/pump2'>Pump 2<br><span class='sub'>" + String(p2 ? "ON" : "OFF") + " • " + modeName(pumpMode[1]) + "</span></a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void refreshPumpOutputs() {
  unsigned long e = timeClient.getEpochTime();
  int now = (e / 60) % 1440;
  bool day = now >= sunriseMinutesUTC && now < sunsetMinutesUTC;
  bool p1 = shouldPumpBeOn(pumpMode[0], day, now, sunsetMinutesUTC, currentTemperature);
  bool p2 = shouldPumpBeOn(pumpMode[1], day, now, sunsetMinutesUTC, currentTemperature);
  digitalWrite(RELAY1, p1 ? HIGH : LOW);
  digitalWrite(RELAY2, p2 ? HIGH : LOW);
  drawPumpRects(p1, p2);
}
void handlePump1() {
  cycleMode(0);
  beep(1600, 80);
  refreshPumpOutputs();
  server.sendHeader("Location", "/");
  server.send(303);
}
void handlePump2() {
  cycleMode(1);
  beep(1600, 80);
  refreshPumpOutputs();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Setup
void setup() {
  Serial.begin(115200);
  prefs.begin("pump", false);
  loadPumpModes();
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(NIGHT_BG);
  tft.setTextColor(TFT_WHITE);
  spiTouch.begin(XPT_SCLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(spiTouch);
  ts.setRotation(1);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  // Start mDNS
  if (MDNS.begin("pondpump")) {
    Serial.println("mDNS responder started: http://pondpump.local");
  } else {
    Serial.println("Error setting up MDNS responder!");
  }

  server.on("/", handleRoot);
  server.on("/pump1", handlePump1);
  server.on("/pump2", handlePump2);
  server.begin();
  timeClient.begin();
  timeClient.update();
  fetchTemperature();
}

// Main loop
void loop() {
  server.handleClient();
  static int lastMinute = -1;
  static bool lastDaytime = false;
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();
  int sec = epochTime % 60, min = (epochTime / 60) % 60, hr = (epochTime / 3600) % 24;
  unsigned long days = epochTime / 86400;
  int year = 1970;
  while (true) {
    int diy = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    if (days >= (unsigned)diy) {
      days -= diy;
      year++;
    } else break;
  }
  int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) dim[1] = 29;
  int month = 1;
  int dleft = days;
  for (int i = 0; i < 12; i++) {
    if (dleft >= dim[i]) {
      dleft -= dim[i];
      month++;
    } else break;
  }
  int day = 1 + dleft;
  if (day != lastDay || month != lastMonth || year != lastYear) {
    calculateSunTimes(day, month, year);
    lastDay = day;
    lastMonth = month;
    lastYear = year;
  }
  int nowM = hr * 60 + min;
  bool dayTime = nowM >= sunriseMinutesUTC && nowM < sunsetMinutesUTC;
  if (dayTime != lastDaytime) {
    tft.fillScreen(dayTime ? DAY_BG : NIGHT_BG);
    if (dayTime) drawSunArc();
    else {
      starsDrawn = false;
      drawStars();
    }
  }
  lastDaytime = dayTime;
  if (millis() - lastTempUpdate > tempInterval || lastTempUpdate == 0) fetchTemperature();
  if (min != lastMinute) {
    lastMinute = min;
    int hrLocal = hr;
    if (isUK_DST(day, month, year)) {
      hrLocal++;
      if (hrLocal >= 24) hrLocal -= 24;
    }
    char buf[32];
    sprintf(buf, "%02d/%02d/%04d %02d:%02d", day, month, year, hrLocal, min);
    tft.fillRect(0, 0, SCREEN_W, 24, dayTime ? DAY_BG : NIGHT_BG);
    tft.setCursor(10, 6);
    tft.print(buf);
    tft.fillRect(SCREEN_W - 120, 0, 120, 24, dayTime ? DAY_BG : NIGHT_BG);
    tft.setCursor(SCREEN_W - 110, 6);
    if (tempValid && currentTemperature > -99) {
      tft.printf("Temp: %.1f", currentTemperature);
      int cx = tft.getCursorX();
      int cy = tft.getCursorY();
      drawDegreeSymbol(cx + 2, cy, TFT_WHITE);
      tft.print(" C");
    } else {
      tft.setTextColor(TFT_RED);
      tft.print("Temp error");
      tft.setTextColor(TFT_WHITE);
    }
    int sRiseH = sunriseMinutesUTC / 60, sRiseM = sunriseMinutesUTC % 60;
    int sSetH = sunsetMinutesUTC / 60, sSetM = sunsetMinutesUTC % 60;
    if (isUK_DST(day, month, year)) {
      sRiseH = (sRiseH + 1) % 24;
      sSetH = (sSetH + 1) % 24;
    }
    char sunStr[48];
    sprintf(sunStr, "Sunrise: %02d:%02d  Sunset: %02d:%02d", sRiseH, sRiseM, sSetH, sSetM);
    tft.fillRect(0, 24, SCREEN_W, 20, dayTime ? DAY_BG : NIGHT_BG);
    int w = tft.textWidth(sunStr);
    tft.setCursor((SCREEN_W - w) / 2, 26);
    tft.print(sunStr);
    float sunEl = -5;
    if (dayTime) {
      float t = (float)(nowM - sunriseMinutesUTC) / (float)(sunsetMinutesUTC - sunriseMinutesUTC);
      sunEl = t * 90.0f;
    }
    updateSunDisplay(sunEl, sunriseMinutesUTC, sunsetMinutesUTC, nowM, sec, dayTime);
    refreshPumpOutputs();
  }
  if (ts.touched() && (millis() - lastTouchMs > touchDebounceMs)) {
    lastTouchMs = millis();
    TS_Point p = ts.getPoint();
    bool c = false;
    if (p.x > 2400) {
      cycleMode(0);
      c = true;
    } else if (p.x < 2300) {
      cycleMode(1);
      c = true;
    }
    if (c) {
      beep(1600, 80);
      refreshPumpOutputs();
    }
  }
  delay(20);
}

// Drawing
void drawSunArc() {
  for (int a = 0; a <= 180; a += 2) {
    float r = a * DEG_TO_RAD;
    int x0 = centerX - radius * cos(r), y0 = centerY - radius * sin(r), x1 = centerX - (radius - 2) * cos(r), y1 = centerY - (radius - 2) * sin(r);
    tft.drawLine(x0, y0, x1, y1, TFT_WHITE);
  }
}
void drawDegreeSymbol(int x, int y, uint16_t c) {
  tft.drawCircle(x, y, 1, c);
}
void drawStars() {
  for (int i = 0; i < STAR_COUNT; i++) {
    starX[i] = random(0, SCREEN_W);
    starY[i] = random(40, SCREEN_H - 40);
    tft.drawPixel(starX[i], starY[i], TFT_WHITE);
  }
  starsDrawn = true;
}
void updateSunDisplay(float sunEl, int sr, int ss, int nowM, int nowS, bool day) {
  if (prevSunX != -1) tft.fillCircle(prevSunX, prevSunY, 12, day ? DAY_BG : NIGHT_BG);
  if (day && sunEl > 0) {
    float t = nowM + nowS / 60.0f;
    float ang = ((t - sr) / (float)(ss - sr)) * 180.0f;
    float r = ang * DEG_TO_RAD;
    int x = centerX - radius * cos(r), y = centerY - radius * sin(r);
    tft.fillCircle(x, y, 8, TFT_YELLOW);
    prevSunX = x;
    prevSunY = y;
    starsDrawn = false;
  } else {
    int mx = centerX, my = centerY - radius / 2;
    tft.fillCircle(mx, my, 10, TFT_WHITE);
    tft.fillCircle(mx + 3, my, 10, NIGHT_BG);
    prevSunX = mx;
    prevSunY = my;
    if (!starsDrawn) drawStars();
  }
  const int bandW = SCREEN_W - 20, bandH = 32, bandX = (SCREEN_W - bandW) / 2, bandY = centerY + 2;
  if (tempValid && currentTemperature > -99 && currentTemperature < 3.0) {
    tft.fillRect(bandX, bandY, bandW, bandH, TFT_RED);
    const char* warn = "FROST WARNING";
    int w1 = tft.textWidth(warn);
    tft.setTextSize(2);
    tft.setCursor(centerX - w1 / 2, bandY + 2);
    tft.print(warn);
    const char* msg = "Pumps disabled";
    int w2 = tft.textWidth(msg);
    tft.setTextSize(1);
    tft.setCursor(centerX - w2 / 2, bandY + 20);
    tft.print(msg);
  } else {
    tft.fillRect(bandX, bandY, bandW, bandH, day ? DAY_BG : NIGHT_BG);
    tft.setTextSize(1);
    if (day && sunEl > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Sun elevation: %.1f", sunEl);
      int w = tft.textWidth(buf);
      int x = centerX - w / 2;
      tft.setCursor(x, bandY + 6);
      tft.print(buf);
      drawDegreeSymbol(x + w + 2, bandY + 7, TFT_WHITE);
    } else {
      const char* nt = "Night time";
      int w = tft.textWidth(nt);
      tft.setCursor(centerX - w / 2, bandY + 6);
      tft.print(nt);
    }
  }
}
void drawPumpRects(bool p1, bool p2) {
  tft.fillRect(rect1X, rectY, rectWidth, rectHeight, p1 ? TFT_GREEN : TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(rect1X + 10, rectY + 6);
  tft.print("PUMP 1");
  tft.setCursor(rect1X + 10, rectY + 18);
  tft.print(modeName(pumpMode[0]));
  tft.fillRect(rect2X, rectY, rectWidth, rectHeight, p2 ? TFT_GREEN : TFT_RED);
  tft.setCursor(rect2X + 10, rectY + 6);
  tft.print("PUMP 2");
  tft.setCursor(rect2X + 10, rectY + 18);
  tft.print(modeName(pumpMode[1]));
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, (WiFi.status() == WL_CONNECTED) ? (tempValid ? TFT_DARKGREY : TFT_RED) : TFT_RED);
  tft.setCursor(4, SCREEN_H - 11);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  if (WiFi.status() == WL_CONNECTED) {
    tft.print(WiFi.localIP().toString());
    if (!tempValid) tft.print(" TEMP ERR");
  } else {
    tft.print("No WiFi");
  }
}
void calculateSunTimes(int d, int m, int y) {
  int N = d;
  int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) dim[1] = 29;
  for (int i = 1; i < m; i++) N += dim[i - 1];
  float decl = 23.45f * sinf((360.0f / 365.0f) * (N - 81) * DEG_TO_RAD);
  float latR = latitude * DEG_TO_RAD, decR = decl * DEG_TO_RAD;
  float cosH = -tanf(latR) * tanf(decR);
  if (cosH > 1) cosH = 1;
  if (cosH < -1) cosH = -1;
  float hA = acosf(cosH) * RAD_TO_DEG;
  sunriseMinutesUTC = (int)(720 - 4 * (longitude + hA));
  sunsetMinutesUTC = (int)(720 - 4 * (longitude - hA));
  if (sunriseMinutesUTC < 0) sunriseMinutesUTC += 1440;
  if (sunsetMinutesUTC < 0) sunsetMinutesUTC += 1440;
}
bool isUK_DST(int d, int m, int y) {
  if (m < 3 || m > 10) return false;
  if (m > 3 && m < 10) return true;
  if (m == 3) {
    int ls = 31;
    for (int dd = 31; dd >= 25; dd--) {
      tm t = {};
      t.tm_mday = dd;
      t.tm_mon = m - 1;
      t.tm_year = y - 1900;
      mktime(&t);
      if (t.tm_wday == 0) {
        ls = dd;
        break;
      }
    }
    return d >= ls;
  }
  if (m == 10) {
    int ls = 31;
    for (int dd = 31; dd >= 25; dd--) {
      tm t = {};
      t.tm_mday = dd;
      t.tm_mon = m - 1;
      t.tm_year = y - 1900;
      mktime(&t);
      if (t.tm_wday == 0) {
        ls = dd;
        break;
      }
    }
    return d < ls;
  }
  return false;
}
void fetchTemperature() {
  if (WiFi.status() != WL_CONNECTED) {
    tempValid = false;
    return;
  }
  HTTPClient http;
  http.begin(tempURL);
  int c = http.GET();
  if (c > 0) {
    String p = http.getString();
    int i = p.indexOf("Outside Temperature");
    if (i != -1) {
      int s = i;
      while (s < p.length() && !isDigit(p[s]) && p[s] != '-') s++;
      int e = s;
      while (e < p.length() && (isDigit(p[e]) || p[e] == '.' || p[e] == '-')) e++;
      if (s < e) {
        currentTemperature = p.substring(s, e).toFloat();
        tempValid = true;
      } else tempValid = false;
    } else tempValid = false;
  } else tempValid = false;
  http.end();
  lastTempUpdate = millis();
}
