#include <WiFi.h>
#include <Preferences.h>
#include "time.h"

Preferences prefs;

const long gmtOffset_sec = -7 * 3600; // Mountain Standard Time
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

void saveCredentials(const char* ssid, const char* pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

bool loadCredentials(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

void connectWiFi() {
  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    Serial.printf("Connecting to %s...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected!");
      return;
    }
  }
  Serial.println("No WiFi credentials saved or connection failed.");
  Serial.println("Type in Serial Monitor: setwifi:SSID,PASSWORD");
}

void setup() {
  Serial.begin(115200);
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
}

void loop() {
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    if (s.startsWith("setwifi:")) {
      s = s.substring(8);
      int comma = s.indexOf(',');
      if (comma > 0) {
        String ssid = s.substring(0, comma);
        String pass = s.substring(comma+1);
        saveCredentials(ssid.c_str(), pass.c_str());
        Serial.println("Saved WiFi credentials. Restarting...");
        ESP.restart();
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.printf("Current time: %02d:%02d:%02d\n",
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
  }
  delay(5000);
}
