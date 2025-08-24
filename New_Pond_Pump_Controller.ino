/*
  Pond Pump Controller
  ESP32 + TFT_eSPI + XPT2046
  -----------------------------------
  Version: 1.1
  Features:
   - NTP time (UTC, with UK DST adjustment)
   - Sun arc & sun elevation (day)
   - Central crescent moon & stars (night)
   - Local temperature via WeeWX
   - Frost warning banner (<3 °C disables pumps)
   - Pump modes: ON / OFF / AUTO / AUTO+1h
   - Pump mode toggle via touch (raw X)
   - Pump mode saved/restored in NVS
   - Display: 320x240, rotation=3
   - Relays: GPIO22 (Pump1), GPIO27 (Pump2)
   - Buzzer: GPIO26
   - Backlight: GPIO21
*/

#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

// ================== WiFi ==================
const char* ssid     = "SKYCE521_Ext";
const char* password = "PNTWTPPFVC";

// ================== Display ==================
TFT_eSPI tft = TFT_eSPI();

// Pins
const int BL_PIN     = 21;  // TFT backlight
const int RELAY1     = 22;  // Pump 1 relay
const int RELAY2     = 27;  // Pump 2 relay
const int BUZZER_PIN = 26;  // Piezo/buzzer pin

// UI colors
#define DAY_BG   TFT_BLUE
#define NIGHT_BG TFT_BLACK

// Screen (rotation=3 => 320w x 240h)
const int SCREEN_W = 320;
const int SCREEN_H = 240;

// Sun arc geometry
const int centerX = 160;
const int centerY = 150;
const int radius  = 100;

// Track previously drawn sun/moon
int prevSunX = -1;
int prevSunY = -1;

// ================== Time (UTC) ==================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // UTC

// UK location
float latitude = 51.8880531;
float longitude = -2.0434736;

// Sunrise/sunset
int sunriseMinutesUTC = 360;
int sunsetMinutesUTC  = 1080;
int lastDay = -1, lastMonth = -1, lastYear = -1;

// ================== Temperature ==================
float currentTemperature = -100.0;
unsigned long lastTempUpdate = 0;
const char* tempURL = "http://weewx.local/weewx/";
const unsigned long tempInterval = 600000UL; // 10 minutes

// ================== Pump Modes ==================
enum PumpMode { MODE_ON = 1, MODE_OFF = 2, MODE_AUTO = 3, MODE_AUTO_EXTEND = 4 };
PumpMode pumpMode[2] = { MODE_AUTO, MODE_AUTO };

const char* modeName(PumpMode m) {
  switch (m) {
    case MODE_ON:          return "ON";
    case MODE_OFF:         return "OFF";
    case MODE_AUTO:        return "AUTO";
    case MODE_AUTO_EXTEND: return "AUTO+1h";
  }
  return "?";
}

// ================== Pump rectangles ==================
const int rectWidth  = 120;
const int rectHeight = 30;
const int gap = 20;
const int margin = (SCREEN_W - 2*rectWidth - gap)/2; // = 30
const int rectY = 200;
const int rect1X = margin;
const int rect2X = margin + rectWidth + gap;

// ================== Touch (XPT2046) ==================
#define XPT_IRQ  36
#define XPT_MOSI 32
#define XPT_MISO 39
#define XPT_SCLK 25
#define XPT_CS   33

SPIClass spiTouch(HSPI);
XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);

// Debounce
unsigned long lastTouchMs = 0;
const unsigned long touchDebounceMs = 300;

// ================== Preferences ==================
Preferences prefs;

// ================== Stars ==================
#define STAR_COUNT 30
int starX[STAR_COUNT], starY[STAR_COUNT];
bool starsDrawn = false;

// ================== Prototypes ==================
bool isUK_DST(int day, int month, int year);
void calculateSunTimes(int day, int month, int year);
void drawSunArc();
void updateSunDisplay(float sunElevation, int sunriseMinutesLocal, int sunsetMinutesLocal,
                      int nowMinutesLocal, int nowSeconds, bool isDaytime);
void fetchTemperature();
void drawPumpRects(bool pumpsOn1, bool pumpsOn2);
void drawDegreeSymbol(int x, int y, uint16_t color);
void drawStars();

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

void beep(int freq, int durationMs) { tone(BUZZER_PIN, freq, durationMs); delay(durationMs); noTone(BUZZER_PIN); }
void cycleMode(int pumpIndex) {
  PumpMode m = pumpMode[pumpIndex];
  switch (m) {
    case MODE_ON:          m = MODE_OFF; break;
    case MODE_OFF:         m = MODE_AUTO; break;
    case MODE_AUTO:        m = MODE_AUTO_EXTEND; break;
    case MODE_AUTO_EXTEND: m = MODE_ON; break;
  }
  pumpMode[pumpIndex] = m;
  beep(2000, 80);
  savePumpModes();
}

bool shouldPumpBeOn(PumpMode mode, bool isDaytimeUTC, int nowMinutesUTC, int sunsetUTCminutes, float tempC) {
  if (!(tempC >= 3.0)) return false; // frost lockout
  switch (mode) {
    case MODE_ON:          return true;
    case MODE_OFF:         return false;
    case MODE_AUTO:        return isDaytimeUTC;
    case MODE_AUTO_EXTEND: return (isDaytimeUTC || (nowMinutesUTC < sunsetUTCminutes + 60));
  }
  return false;
}

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  prefs.begin("pump", false);
  loadPumpModes();

  pinMode(BL_PIN, OUTPUT);   digitalWrite(BL_PIN, HIGH);
  pinMode(RELAY1, OUTPUT);   pinMode(RELAY2, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(NIGHT_BG);
  tft.setTextColor(TFT_WHITE);

  spiTouch.begin(XPT_SCLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(spiTouch);
  ts.setRotation(1);

  WiFi.begin(ssid, password);
  timeClient.begin();
  timeClient.update();
  fetchTemperature();
}

// ================== Main Loop ==================
void loop() {
  static int lastMinute = -1;
  static bool lastDaytime = false;
  static bool backgroundDrawn = false;

  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();

  int second = epochTime % 60;
  int minute = (epochTime / 60) % 60;
  int hour   = (epochTime / 3600) % 24;

  // Convert epoch -> Y/M/D
  unsigned long days = epochTime / 86400;
  int year = 1970;
  while (true) {
    int diy = (year%4==0 && (year%100!=0 || year%400==0)) ? 366 : 365;
    if (days >= (unsigned)diy) { days -= diy; year++; } else break;
  }
  int month = 1;
  int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (year%4==0 && (year%100!=0 || year%400==0)) dim[1]=29;
  int day = 1;
  for (int i=0;i<12;i++){ if (days >= (unsigned)dim[i]) { days -= dim[i]; month++; } else { day += days; break; } }

  if (day!=lastDay || month!=lastMonth || year!=lastYear) {
    calculateSunTimes(day,month,year);
    lastDay=day; lastMonth=month; lastYear=year;
  }

  int nowMinutesUTC = hour*60 + minute;
  bool isDaytime = nowMinutesUTC >= sunriseMinutesUTC && nowMinutesUTC < sunsetMinutesUTC;

  if (millis() - lastTempUpdate > tempInterval || lastTempUpdate == 0) fetchTemperature();

  if (minute != lastMinute) {
    lastMinute = minute;

    int hourLocal = hour;
    if (isUK_DST(day, month, year)) { hourLocal++; if (hourLocal>=24) hourLocal-=24; }

    if (isDaytime != lastDaytime || !backgroundDrawn) {
      tft.fillScreen(isDaytime ? DAY_BG : NIGHT_BG);
      if (isDaytime) drawSunArc();
      else { starsDrawn = false; drawStars(); }
      backgroundDrawn = true;
    }
    lastDaytime = isDaytime;

    // Date/time header
    char dateTimeStr[25];
    sprintf(dateTimeStr, "%02d/%02d/%04d %02d:%02d", day, month, year, hourLocal, minute);
    tft.fillRect(0, 0, SCREEN_W, 24, isDaytime ? DAY_BG : NIGHT_BG);
    tft.setCursor(10, 6); tft.setTextSize(1); tft.print(dateTimeStr);

    // Temp top-right
    tft.fillRect(SCREEN_W - 120, 0, 120, 24, isDaytime ? DAY_BG : NIGHT_BG);
    tft.setCursor(SCREEN_W - 110, 6); tft.setTextSize(1);
    if (currentTemperature > -99) {
      tft.printf("Temp: %.1f", currentTemperature);
      int cx = tft.getCursorX();
      int cy = tft.getCursorY();
      drawDegreeSymbol(cx + 2, cy, TFT_WHITE);
      tft.print(" C");
    } else {
      tft.print("Temp: --.-C");
    }

    // Sunrise/Sunset centered
    int sRiseH = sunriseMinutesUTC/60, sRiseM = sunriseMinutesUTC%60;
    int sSetH  = sunsetMinutesUTC/60,  sSetM  = sunsetMinutesUTC%60;
    if (isUK_DST(day,month,year)) { sRiseH=(sRiseH+1)%24; sSetH=(sSetH+1)%24; }
    char sunTimesStr[48];
    sprintf(sunTimesStr, "Sunrise: %02d:%02d  Sunset: %02d:%02d", sRiseH, sRiseM, sSetH, sSetM);
    tft.fillRect(0, 24, SCREEN_W, 20, isDaytime ? DAY_BG : NIGHT_BG);
    int w = tft.textWidth(sunTimesStr);
    tft.setCursor((SCREEN_W - w)/2, 26);
    tft.print(sunTimesStr);

    // Sun elevation or frost banner
    float sunElevation = -5;
    if (isDaytime) {
      float t = (float)(nowMinutesUTC - sunriseMinutesUTC) / (float)(sunsetMinutesUTC - sunriseMinutesUTC);
      sunElevation = t * 90.0f;
    }
    updateSunDisplay(sunElevation, sunriseMinutesUTC, sunsetMinutesUTC, nowMinutesUTC, second, isDaytime);

    // Relays with safety
    bool p1On = shouldPumpBeOn(pumpMode[0], isDaytime, nowMinutesUTC, sunsetMinutesUTC, currentTemperature);
    bool p2On = shouldPumpBeOn(pumpMode[1], isDaytime, nowMinutesUTC, sunsetMinutesUTC, currentTemperature);
    digitalWrite(RELAY1, p1On ? HIGH : LOW);
    digitalWrite(RELAY2, p2On ? HIGH : LOW);
    drawPumpRects(p1On, p2On);
  }

  // Touch handling (FIXED to update immediately)
  if (ts.touched() && (millis() - lastTouchMs > touchDebounceMs)) {
    lastTouchMs = millis();
    TS_Point p = ts.getPoint();

    bool changed = false;
    if (p.x > 2400) { cycleMode(0); changed = true; }
    else if (p.x < 2300) { cycleMode(1); changed = true; }

    if (changed) {
      unsigned long e = timeClient.getEpochTime();
      int nowUTCmin = (e/60) % 1440;
      bool isDayNow = (nowUTCmin >= sunriseMinutesUTC && nowUTCmin < sunsetMinutesUTC);

      bool p1On = shouldPumpBeOn(pumpMode[0], isDayNow, nowUTCmin, sunsetMinutesUTC, currentTemperature);
      bool p2On = shouldPumpBeOn(pumpMode[1], isDayNow, nowUTCmin, sunsetMinutesUTC, currentTemperature);

      digitalWrite(RELAY1, p1On ? HIGH : LOW);
      digitalWrite(RELAY2, p2On ? HIGH : LOW);

            drawPumpRects(p1On, p2On);
    }
  }

  delay(25);
}

// ================== Drawing & Support Functions ==================
void drawSunArc() {
  for (int angleDeg = 0; angleDeg <= 180; angleDeg += 2) {
    float rad = angleDeg * DEG_TO_RAD;
    int x0 = centerX - radius     * cos(rad);
    int y0 = centerY - radius     * sin(rad);
    int x1 = centerX - (radius-2) * cos(rad);
    int y1 = centerY - (radius-2) * sin(rad);
    tft.drawLine(x0, y0, x1, y1, TFT_WHITE);
  }
}

void drawDegreeSymbol(int x, int y, uint16_t color) { tft.drawCircle(x, y, 1, color); }

void drawStars() {
  for (int i=0;i<STAR_COUNT;i++) {
    starX[i] = random(0, SCREEN_W);
    starY[i] = random(40, SCREEN_H-40);
    tft.drawPixel(starX[i], starY[i], TFT_WHITE);
  }
  starsDrawn = true;
}

void updateSunDisplay(float sunElevation, int sunriseMinutesLocal, int sunsetMinutesLocal,
                      int nowMinutesLocal, int nowSeconds, bool isDaytime) {
  if (prevSunX != -1) tft.fillCircle(prevSunX, prevSunY, 12, isDaytime ? DAY_BG : NIGHT_BG);

  if (isDaytime && sunElevation > 0) {
    float nowTime = nowMinutesLocal + nowSeconds / 60.0f;
    float angle = ((nowTime - sunriseMinutesLocal) / (float)(sunsetMinutesLocal - sunriseMinutesLocal)) * 180.0f;
    float rad = angle * DEG_TO_RAD;
    int sunX = centerX - radius * cos(rad);
    int sunY = centerY - radius * sin(rad);
    tft.fillCircle(sunX, sunY, 8, TFT_YELLOW);
    prevSunX = sunX; prevSunY = sunY;
    starsDrawn = false;
  } else {
    int moonX = centerX;
    int moonY = centerY - radius/2;
    tft.fillCircle(moonX, moonY, 10, TFT_WHITE);
    tft.fillCircle(moonX + 3, moonY, 10, NIGHT_BG);
    prevSunX = moonX; prevSunY = moonY;
    if (!starsDrawn) drawStars();
  }

  const int bandW = SCREEN_W - 20;
  const int bandH = 32;
  const int bandX = (SCREEN_W - bandW)/2;
  const int bandY = centerY + 2;

  if (currentTemperature > -99 && currentTemperature < 3.0f) {
    tft.fillRect(bandX, bandY, bandW, bandH, TFT_RED);
    const char* warn = "FROST WARNING";
    int w1 = tft.textWidth(warn);
    tft.setTextSize(2);
    tft.setCursor(centerX - w1/2, bandY + 2); tft.print(warn);
    const char* msg = "Pumps disabled";
    int w2 = tft.textWidth(msg);
    tft.setTextSize(1);
    tft.setCursor(centerX - w2/2, bandY + 20); tft.print(msg);
  } else {
    tft.fillRect(bandX, bandY, bandW, bandH, isDaytime ? DAY_BG : NIGHT_BG);
    tft.setTextSize(1);
    if (isDaytime && sunElevation > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Sun elevation: %.1f", sunElevation);
      int w = tft.textWidth(buf);
      int textX = centerX - w/2;
      tft.setCursor(textX, bandY + 6);
      tft.print(buf);
      drawDegreeSymbol(textX + w + 2, bandY + 7, TFT_WHITE);
    } else {
      const char* nt = "Night time";
      int w = tft.textWidth(nt);
      tft.setCursor(centerX - w/2, bandY + 6); tft.print(nt);
    }
  }
}

void drawPumpRects(bool pumpsOn1, bool pumpsOn2) {
  tft.fillRect(rect1X, rectY, rectWidth, rectHeight, pumpsOn1 ? TFT_GREEN : TFT_RED);
  tft.setTextColor(TFT_WHITE); tft.setTextSize(1);
  tft.setCursor(rect1X + 10, rectY + 6);  tft.print("PUMP 1");
  tft.setCursor(rect1X + 10, rectY + 18); tft.print(modeName(pumpMode[0]));

  tft.fillRect(rect2X, rectY, rectWidth, rectHeight, pumpsOn2 ? TFT_GREEN : TFT_RED);
  tft.setCursor(rect2X + 10, rectY + 6);  tft.print("PUMP 2");
  tft.setCursor(rect2X + 10, rectY + 18); tft.print(modeName(pumpMode[1]));
}

void calculateSunTimes(int day, int month, int year) {
  int N = day;
  int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (year%4==0 && (year%100!=0 || year%400==0)) dim[1]=29;
  for (int m=1; m<month; m++) N += dim[m-1];
  float decl = 23.45f * sinf((360.0f/365.0f) * (N - 81) * DEG_TO_RAD);
  float latRad = latitude * DEG_TO_RAD;
  float declRad = decl * DEG_TO_RAD;
  float cosH = -tanf(latRad) * tanf(declRad);
  if (cosH >  1.0f) cosH =  1.0f;
  if (cosH < -1.0f) cosH = -1.0f;
  float hourAngle = acosf(cosH) * RAD_TO_DEG;
  sunriseMinutesUTC = (int)(720 - 4 * (longitude + hourAngle));
  sunsetMinutesUTC  = (int)(720 - 4 * (longitude - hourAngle));
  if (sunriseMinutesUTC < 0) sunriseMinutesUTC += 1440;
  if (sunsetMinutesUTC  < 0) sunsetMinutesUTC  += 1440;
}

bool isUK_DST(int day, int month, int year) {
  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;
  if (month == 3) {
    int lastSunday = 31;
    for (int d=31; d>=25; d--) {
      tm t = {}; t.tm_mday = d; t.tm_mon = month-1; t.tm_year = year-1900;
      mktime(&t);
      if (t.tm_wday == 0){ lastSunday=d; break; }
    }
    return day >= lastSunday;
  }
  if (month == 10) {
    int lastSunday = 31;
    for (int d=31; d>=25; d--) {
      tm t = {}; t.tm_mday = d; t.tm_mon = month-1; t.tm_year = year-1900;
      mktime(&t);
      if (t.tm_wday == 0){ lastSunday=d; break; }
    }
    return day < lastSunday;
  }
  return false;
}

void fetchTemperature() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(tempURL);
  int httpCode = http.GET();
  if (httpCode > 0) {
    String payload = http.getString();
    int index = payload.indexOf("Outside Temperature");
    if (index != -1) {
      int start = index;
      while (start < payload.length() && !isDigit(payload[start]) && payload[start] != '-') start++;
      int end = start;
      while (end < payload.length() && (isDigit(payload[end]) || payload[end]=='.' || payload[end]=='-')) end++;
      if (start < end) currentTemperature = payload.substring(start, end).toFloat();
    }
  }
  http.end();
  lastTempUpdate = millis();
}
