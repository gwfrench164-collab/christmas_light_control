#include <WiFi.h>
#include <Preferences.h>
#include "time.h"

Preferences prefs;

// Timezone (Mountain Standard Time)
const long gmtOffset_sec = -7 * 3600;
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// Relay pins
const int RELAY_PINS[8] = {12, 13, 15, 25, 26, 27, 32, 33};
const bool RELAY_ACTIVE_LOW = false;

// Schedule
int on_h = 18, on_m = 0;   // default ON 6:00 PM
int off_h = 22, off_m = 0; // default OFF 10:00 PM
bool relaysEnabled = false;

// Pattern selection
enum Pattern { CHASE, WAVE, RANDOM };
Pattern currentPattern = CHASE;

void setRelay(int idx, bool on) {
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
  else digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
}

void allOff() {
  for (int i = 0; i < 8; i++) setRelay(i, false);
}

void runPattern() {
  switch (currentPattern) {
    case CHASE:
      for (int i = 0; i < 8; i++) {
        allOff();
        setRelay(i, true);
        delay(200);
      }
      break;
    case WAVE:
      allOff();
      for (int i = 0; i < 8; i++) {
        setRelay(i, true);
        delay(150);
      }
      for (int i = 7; i >= 0; i--) {
        setRelay(i, false);
        delay(150);
      }
      break;
    case RANDOM:
      allOff();
      int r = random(0, 8);
      setRelay(r, true);
      delay(300);
      break;
  }
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
  Serial.printf("Pattern: %s\n", currentPattern == CHASE ? "CHASE" :
                                currentPattern == WAVE ? "WAVE" : "RANDOM");
  Serial.printf("Relays are %s\n", relaysEnabled ? "ENABLED" : "DISABLED");
  Serial.println("----------------");
}

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
      on_h = h; on_m = m;
      Serial.printf("Saved ON time %02d:%02d\n", on_h, on_m);
    } else Serial.println("Bad format. Use seton:HH:MM");
    return;
  }
  if (s.startsWith("setoff:")) {
    int h,m;
    if (parseTime(s.substring(7), h, m)) {
      off_h = h; off_m = m;
      Serial.printf("Saved OFF time %02d:%02d\n", off_h, off_m);
    } else Serial.println("Bad format. Use setoff:HH:MM");
    return;
  }
  if (s.equalsIgnoreCase("onnow")) {
    relaysEnabled = true;
    Serial.println("Relays ENABLED now");
    return;
  }
  if (s.equalsIgnoreCase("offnow")) {
    relaysEnabled = false;
    allOff();
    Serial.println("Relays DISABLED now");
    return;
  }
  if (s.equalsIgnoreCase("status")) {
    printStatus();
    return;
  }
  if (s.startsWith("pattern:")) {
    String p = s.substring(8);
    p.toUpperCase();
    if (p == "CHASE") currentPattern = CHASE;
    else if (p == "WAVE") currentPattern = WAVE;
    else if (p == "RANDOM") currentPattern = RANDOM;
    else Serial.println("Unknown pattern. Use CHASE, WAVE, RANDOM");
    printStatus();
    return;
  }
  Serial.println("Unknown command. Use seton:, setoff:, onnow, offnow, status, pattern:");
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    setRelay(i, false);
  }

  // Load WiFi creds
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Connecting to %s...\n", ssid.c_str());
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nConnected!");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }

  Serial.println("Ready. Commands: seton:HH:MM  setoff:HH:MM  onnow  offnow  status  pattern:NAME");
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

      bool shouldBeOn = (on_min <= off_min)
        ? (now_min >= on_min && now_min < off_min)
        : (now_min >= on_min || now_min < off_min);

      relaysEnabled = shouldBeOn;
    }
  }

  if (relaysEnabled) {
    runPattern();
  } else {
    allOff();
    delay(500);
  }
}
