#include <WiFi.h>
#include <Preferences.h>
#include "time.h"

Preferences prefs;

// Timezone (Mountain Standard Time)
const long gmtOffset_sec = -7 * 3600;
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// Relay pin
const int RELAY_PIN = 13;
const bool RELAY_ACTIVE_LOW = false; // set true if your relay energizes on LOW

// Stored keys
const char* PREF_NS = "config";
const char* KEY_ON_H = "on_h";
const char* KEY_ON_M = "on_m";
const char* KEY_OFF_H = "off_h";
const char* KEY_OFF_M = "off_m";

// runtime cached schedule
int on_h = 18, on_m = 0;   // default ON 18:00 (6pm)
int off_h = 22, off_m = 0; // default OFF 22:00 (10pm)
bool relayState = false;

void saveSchedule() {
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_ON_H, on_h);
  prefs.putUInt(KEY_ON_M, on_m);
  prefs.putUInt(KEY_OFF_H, off_h);
  prefs.putUInt(KEY_OFF_M, off_m);
  prefs.end();
}

void loadSchedule() {
  prefs.begin(PREF_NS, true);
  on_h = prefs.getUInt(KEY_ON_H, on_h);
  on_m = prefs.getUInt(KEY_ON_M, on_m);
  off_h = prefs.getUInt(KEY_OFF_H, off_h);
  off_m = prefs.getUInt(KEY_OFF_M, off_m);
  prefs.end();
}

void setRelay(bool on) {
  relayState = on;
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  else digitalWrite(RELAY_PIN, on ? HIGH : LOW);
}

String two(int v) {
  char b[6];
  sprintf(b, "%02d", v);
  return String(b);
}

void printStatus() {
  struct tm timeinfo;
  Serial.println("---- STATUS ----");
  if (getLocalTime(&timeinfo)) {
    Serial.printf("Local time: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    Serial.println("Local time: not available");
  }
  Serial.printf("ON time: %s:%s\n", two(on_h).c_str(), two(on_m).c_str());
  Serial.printf("OFF time: %s:%s\n", two(off_h).c_str(), two(off_m).c_str());
  Serial.printf("Relay is %s\n", relayState ? "ON" : "OFF");
  Serial.println("----------------");
}

// parse HH:MM from string; returns true on success
bool parseTime(String s, int &h, int &m) {
  s.trim();
  int colon = s.indexOf(':');
  if (colon < 0) return false;
  int hh = s.substring(0, colon).toInt();
  int mm = s.substring(colon + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = hh; m = mm;
  return true;
}

void processSerialLine(String s) {
  s.trim();
  if (s.length() == 0) return;
  if (s.startsWith("seton:")) {
    int h,m;
    if (parseTime(s.substring(6), h, m)) {
      on_h = h; on_m = m; saveSchedule();
      Serial.printf("Saved ON time %02d:%02d\n", on_h, on_m);
      printStatus();
    } else Serial.println("bad format. Use seton:HH:MM (24-hour)");
    return;
  }
  if (s.startsWith("setoff:")) {
    int h,m;
    if (parseTime(s.substring(7), h, m)) {
      off_h = h; off_m = m; saveSchedule();
      Serial.printf("Saved OFF time %02d:%02d\n", off_h, off_m);
      printStatus();
    } else Serial.println("bad format. Use setoff:HH:MM (24-hour)");
    return;
  }
  if (s.equalsIgnoreCase("onnow")) {
    setRelay(true);
    Serial.println("Relay ON now");
    printStatus();
    return;
  }
  if (s.equalsIgnoreCase("offnow")) {
    setRelay(false);
    Serial.println("Relay OFF now");
    printStatus();
    return;
  }
  if (s.equalsIgnoreCase("status")) {
    printStatus();
    return;
  }
  Serial.println("Unknown command. Use seton:, setoff:, onnow, offnow, status");
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // relay setup
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  // load schedule from prefs
  loadSchedule();

  // connect WiFi
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Connecting to %s...\n", ssid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    } else {
      Serial.println("\nWiFi connect failed.");
    }
  } else {
    Serial.println("No WiFi credentials saved. Use previous sketch to set them.");
  }

  Serial.println("Ready. Commands: seton:HH:MM  setoff:HH:MM  onnow  offnow  status");
  printStatus();
}

unsigned long lastCheckMs = 0;

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    processSerialLine(line);
  }

  if (millis() - lastCheckMs > 10000) {
    lastCheckMs = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int now_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int on_min = on_h * 60 + on_m;
      int off_min = off_h * 60 + off_m;

      bool shouldBeOn;
      if (on_min <= off_min) {
        shouldBeOn = (now_min >= on_min && now_min < off_min);
      } else {
        shouldBeOn = (now_min >= on_min || now_min < off_min);
      }

      if (shouldBeOn && !relayState) {
        setRelay(true);
        Serial.println("Schedule: switching RELAY ON");
      } else if (!shouldBeOn && relayState) {
        setRelay(false);
        Serial.println("Schedule: switching RELAY OFF");
      }
    }
  }
}
