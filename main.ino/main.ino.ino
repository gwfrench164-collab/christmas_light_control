#include <WiFi.h>
#include <Preferences.h>
#include "time.h"
#include <WebServer.h>

Preferences prefs;
WebServer server(80);

// Timezone
const long gmtOffset_sec = -7 * 3600;
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// Relay pins
const int RELAY_PINS[8] = {12, 13, 15, 25, 26, 27, 32, 33};
const bool RELAY_ACTIVE_LOW = false;

// Schedule
int on_h = 18, on_m = 0;
int off_h = 22, off_m = 0;
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

// Web handlers
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<style>";
  html += "body { font-family: Arial; text-align: center; }";
  html += "button { display: block; width: 200px; margin: 15px auto; padding: 15px; font-size: 18px; }";
  html += "</style></head><body>";
  html += "<h1>Christmas Light Control</h1>";
  html += "<form action='/on'><button>Turn ON</button></form>";
  html += "<form action='/off'><button>Turn OFF</button></form>";
  html += "<form action='/status'><button>Status</button></form>";
  html += "<form action='/pattern?name=CHASE'><button>Pattern: CHASE</button></form>";
  html += "<form action='/pattern?name=WAVE'><button>Pattern: WAVE</button></form>";
  html += "<form action='/pattern?name=RANDOM'><button>Pattern: RANDOM</button></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}


void handleOn() {
  relaysEnabled = true;
  server.send(200, "text/plain", "Relays ON");
}

void handleOff() {
  relaysEnabled = false;
  allOff();
  server.send(200, "text/plain", "Relays OFF");
}

void handleStatus() {
  struct tm timeinfo;
  String msg = "Status:\n";
  if (getLocalTime(&timeinfo)) {
    msg += "Local time: " + String(timeinfo.tm_hour) + ":" + String(timeinfo.tm_min) + "\n";
  }
  msg += "ON time: " + String(on_h) + ":" + String(on_m) + "\n";
  msg += "OFF time: " + String(off_h) + ":" + String(off_m) + "\n";
  msg += "Pattern: " + String(currentPattern == CHASE ? "CHASE" :
                              currentPattern == WAVE ? "WAVE" : "RANDOM") + "\n";
  msg += "Relays: " + String(relaysEnabled ? "ENABLED" : "DISABLED") + "\n";
  server.send(200, "text/plain", msg);
}

void handlePattern() {
  String name = server.arg("name");
  name.toUpperCase();
  if (name == "CHASE") currentPattern = CHASE;
  else if (name == "WAVE") currentPattern = WAVE;
  else if (name == "RANDOM") currentPattern = RANDOM;
  server.send(200, "text/plain", "Pattern set to " + name);
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
    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/status", handleStatus);
  server.on("/pattern", handlePattern);
  server.begin();
}

unsigned long lastCheckMs = 0;

void loop() {
  server.handleClient();

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
