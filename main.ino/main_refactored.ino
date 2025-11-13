/*
  Christmas Light Control (ESP32) — Refactored (HTTPS + ArduinoJson + Captive Portal)

  Features:
    - Web UI to control 8 relays: ON/OFF/Auto, Patterns (CHASE/WAVE/RANDOM), Speed
    - Non-blocking pattern engine (no delay() in the main loop)
    - Sunset-based ON time from OpenWeather (HTTPS) parsed with ArduinoJson
    - User-configurable Sunset offset (-180..+180 minutes)
    - DST-aware timezone via TZ rule (MST/MDT)
    - Stores settings in NVS (Preferences); writes only when values change
    - Wi-Fi connect timeout with SoftAP + Captive Portal for first-time setup (SSID/PASS/API key)
    - mDNS (http://xmas.local/)
    - JSON status endpoint (/status.json)

  Notes:
    - The OpenWeather API key is NOT hardcoded. Enter it on the setup page when the captive portal appears,
      or later via http://xmas.local/setup (when connected).
    - HTTPS uses setInsecure() for simplicity; for production you can pin a certificate or fingerprint.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoJson.h>

// ----------------------------- Hardware config -----------------------------
const int RELAY_PINS[8] = {12, 13, 15, 25, 26, 27, 32, 33};
const bool RELAY_ACTIVE_LOW = false; // set true if your relay boards are active-low

// ----------------------------- Web / Networking ----------------------------
WebServer server(80);
DNSServer dnsServer;                 // for captive portal
Preferences prefs;

// mDNS name
const char* MDNS_NAME = "xmas";

// Captive portal AP
String apSSID;
IPAddress apIP(192, 168, 4, 1);
const byte DNS_PORT = 53;

// ----------------------------- Location (for OpenWeather) ------------------
const char* latitude  = "41.3114";   // Laramie, WY (edit if needed)
const char* longitude = "-105.5911"; // Laramie, WY (edit if needed)

// ----------------------------- Time / TZ -----------------------------------
const char* ntpServer = "pool.ntp.org";
// Mountain time with DST (US): M3.2.0/2 (2am 2nd Sunday in March), M11.1.0/2 (2am 1st Sunday in Nov)
const char* TZ_RULE = "MST7MDT,M3.2.0/2,M11.1.0/2";

// ----------------------------- App state -----------------------------------
enum Pattern { CHASE, WAVE, RANDOM };
Pattern currentPattern = CHASE;
int patternSpeed = 200;              // ms per step (50..1000)
bool relaysEnabled = false;          // current power to relays
bool manualOverride = false;         // manual vs. auto schedule
bool useSunset = true;               // if true, ON time tracks sunset daily
int  sunsetOffsetMin = 0;            // -180..+180
int  on_h = 18, on_m = 0;            // manual schedule ON  time (hh:mm)
int  off_h = 22, off_m = 0;          // manual schedule OFF time (hh:mm)

// Wi-Fi credentials and OpenWeather key (stored in NVS, not hardcoded)
String wifiSSID, wifiPASS, weatherApiKey;

// ----------------------------- Timers --------------------------------------
unsigned long lastSunsetUpdate = 0;  // ms since boot
unsigned long lastScheduleCheck = 0;

// Pattern timing state (non-blocking)
uint8_t      patIndex = 0;
unsigned long lastPatStep = 0;
bool alreadyOff = false;

// ----------------------------- Utilities -----------------------------------
inline void setRelay(int idx, bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
  }
}

void allOff() {
  for (int i = 0; i < 8; i++) setRelay(i, false);
}

template <typename T>
bool putIfChanged(Preferences& p, const char* key, const T& value, const T& current) {
  if (value == current) return false; // unchanged
  // Overload resolution: store via appropriate put* API
  if constexpr (std::is_same<T, bool>::value) {
    p.putBool(key, value);
  } else if constexpr (std::is_same<T, int>::value) {
    p.putInt(key, value);
  } else if constexpr (std::is_same<T, String>::value) {
    p.putString(key, value);
  }
  return true;
}

bool parseTimeStr(String s, int &h, int &m) {
  s.trim();
  int colon = s.indexOf(':');
  if (colon < 0) return false;
  int hh = s.substring(0, colon).toInt();
  int mm = s.substring(colon + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = hh; m = mm; return true;
}

// ----------------------------- Pattern engine (non-blocking) ---------------
void patternStepCHASE() {
  allOff();
  setRelay(patIndex, true);
  patIndex = (patIndex + 1) % 8;
}

void patternStepWAVE() {
  static bool filling = true;
  static uint8_t level = 0;
  allOff();
  if (filling) {
    for (uint8_t i = 0; i <= level && i < 8; i++) setRelay(i, true);
    if (++level >= 8) { filling = false; level = 7; }
  } else {
    for (uint8_t i = 0; i <= level && i < 8; i++) setRelay(i, true);
    if (level-- == 0)   { filling = true;  level = 0; }
  }
}

void patternStepRANDOM() {
  allOff();
  setRelay(random(0,8), true);
}

void stepPatternIfDue() {
  unsigned long now = millis();
  if ((unsigned long)(now - lastPatStep) < (unsigned long)patternSpeed) return;
  lastPatStep = now;

  switch (currentPattern) {
    case CHASE:  patternStepCHASE();  break;
    case WAVE:   patternStepWAVE();   break;
    case RANDOM: patternStepRANDOM(); break;
  }
}

// ----------------------------- OpenWeather (HTTPS + ArduinoJson) ----------
bool updateSunsetTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (weatherApiKey.length() == 0)    return false;

  WiFiClientSecure client;
  client.setInsecure(); // For simplicity. For production, pin the certificate/fingerprint.

  HTTPClient http;
  String url = "https://api.openweathermap.org/data/2.5/weather?lat=" +
               String(latitude) + "&lon=" + String(longitude) +
               "&appid=" + weatherApiKey;

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Weather API error: %d\n", httpCode);
    http.end();
    return false;
  }

  // Parse JSON
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  // sys.sunset is Unix UTC
  if (!doc.containsKey("sys") || !doc["sys"].containsKey("sunset")) {
    Serial.println("JSON missing sys.sunset");
    return false;
  }

  time_t sunsetUnix = (time_t) doc["sys"]["sunset"].as<long>();
  sunsetUnix += (time_t)sunsetOffsetMin * 60; // apply user offset

  struct tm *tminfo = localtime(&sunsetUnix);
  if (!tminfo) {
    Serial.println("localtime() failed for sunset");
    return false;
  }

  on_h = tminfo->tm_hour;
  on_m = tminfo->tm_min;
  lastSunsetUpdate = millis();

  Serial.printf("Updated ON to sunset%+d: %02d:%02d\n", sunsetOffsetMin, on_h, on_m);
  return true;
}

// ----------------------------- Scheduling ----------------------------------
bool computeShouldBeOn() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return relaysEnabled; // keep current state if time unknown
  int now_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int on_min  = on_h * 60 + on_m;
  int off_min = off_h * 60 + off_m;

  // If on <= off: active in [on, off)
  // If on > off : active across midnight, i.e. now >= on OR now < off
  if (on_min <= off_min) {
    return (now_min >= on_min && now_min < off_min);
  } else {
    return (now_min >= on_min || now_min < off_min);
  }
}

// ----------------------------- Captive Portal / Setup ----------------------
String htmlHeader() {
  return F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<style>"
           "body{font-family:Arial;margin:10px;background:#f9f9f9}"
           ".section{background:#fff;padding:12px;margin-bottom:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1)}"
           "h1,h2{font-size:1.2em;margin-top:0}"
           "button{display:block;width:100%;padding:12px;margin:6px 0;font-size:1em;border:none;border-radius:6px;background:#0078d7;color:#fff}"
           "button:active{background:#005a9e}"
           "input,select{width:100%;padding:8px;margin:6px 0;font-size:1em;border:1px solid #ccc;border-radius:6px}"
           "#msg{white-space:pre-line;border:1px solid #ccc;padding:10px;border-radius:6px;background:#fff}"
           "</style></head><body>");
}
String htmlFooter() { return F("</body></html>"); }

void handleRootUI() {
  // Main control UI
  String html = htmlHeader();
  html += F("<div class='section'><h1>Christmas Light Control</h1>"
            "<div id='msg'>Status appears here...</div></div>");

  html += F("<div class='section'><h2>Power</h2>"
            "<button onclick=\"fetch('/on').then(r=>r.text()).then(t=>msg.innerText=t)\">Turn ON (Manual)</button>"
            "<button onclick=\"fetch('/off').then(r=>r.text()).then(t=>msg.innerText=t)\">Turn OFF (Manual)</button>"
            "<button onclick=\"fetch('/auto').then(r=>r.text()).then(t=>msg.innerText=t)\">Return to Auto</button>"
            "</div>");

  html += F("<div class='section'><h2>Patterns</h2>"
            "<button onclick=\"fetch('/pattern?name=CHASE').then(r=>r.text()).then(t=>msg.innerText=t)\">CHASE</button>"
            "<button onclick=\"fetch('/pattern?name=WAVE').then(r=>r.text()).then(t=>msg.innerText=t)\">WAVE</button>"
            "<button onclick=\"fetch('/pattern?name=RANDOM').then(r=>r.text()).then(t=>msg.innerText=t)\">RANDOM</button>"
            "</div>");

  html += "<div class='section'><h2>Pattern Speed</h2>"
          "<input type='range' min='50' max='1000' value='" + String(patternSpeed) + "' id='speed' "
          "oninput=\"document.getElementById('v').innerText=this.value\">"
          "<div>Speed: <span id='v'>" + String(patternSpeed) + "</span> ms</div>"
          "<button onclick=\"fetch('/setspeed?val='+document.getElementById('speed').value).then(r=>r.text()).then(t=>msg.innerText=t)\">Set Speed</button>"
          "</div>";

  html += F("<div class='section'><h2>Schedule</h2>"
            "<input id='on' placeholder='HH:MM'><input id='off' placeholder='HH:MM'>"
            "<button onclick=\"fetch('/setschedule?on='+on.value+'&off='+off.value).then(r=>r.text()).then(t=>msg.innerText=t)\">Save Schedule</button>"
            "</div>");

  html += "<div class='section'><h2>Sunset Scheduling</h2>"
          "<button onclick=\"fetch('/sunsetmode?enable=1').then(r=>r.text()).then(t=>msg.innerText=t)\">Enable Sunset Mode</button>"
          "<button onclick=\"fetch('/sunsetmode?enable=0').then(r=>r.text()).then(t=>msg.innerText=t)\">Disable Sunset Mode</button>"
          "<label>Offset (minutes, -180..+180)</label>"
          "<input type='number' id='offset' value='" + String(sunsetOffsetMin) + "'>"
          "<button onclick=\"fetch('/setSunsetOffset?min='+document.getElementById('offset').value).then(r=>r.text()).then(t=>msg.innerText=t)\">Save Offset</button>"
          "</div>";

  html += F("<div class='section'><h2>Setup (Wi‑Fi & API Key)</h2>"
            "/setup<button>Open Setup Page</button></a>"
            "</div>");

  html += F("<script>const msg=document.getElementById('msg');"
            "function refresh(){fetch('/status').then(r=>r.text()).then(t=>msg.innerText=t)}"
            "setInterval(refresh,4000);window.onload=refresh;</script>");

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleSetupPage() {
  // Show current SSID (masked), provide fields to change Wi-Fi and API key
  String html = htmlHeader();
  html += F("<div class='section'><h1>Setup</h1>"
            "<p>Enter Wi‑Fi credentials and your OpenWeather API key. Device will reboot after saving.</p>"
            "<form action='/save' method='‑Fi SSID</label><input name='ssid' placeholder='SSID' value='");
  html += wifiSSID;
  html += F("'>"
            "<label>Wi‑Fi Password</label><input name='pass' type='password' placeholder='Password' value=''>"
            "<label>OpenWeather API Key</label><input name='owm' placeholder='Your API key' value='");
  html += (weatherApiKey.length() ? String(weatherApiKey.length(), '*') : "");
  html += F("'>"
            "<button type='submit'>Save & Reboot</button>"
            "</form></div>");
  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleSaveSetup() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String owm  = server.arg("owm");

  prefs.begin("wifi", false);
  if (ssid.length()) prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("pass", pass);
  prefs.end();

  prefs.begin("owm", false);
  if (owm.length()) prefs.putString("key", owm);
  prefs.end();

  server.send(200, "text/plain", "Saved. Rebooting in 2 seconds...");
  delay(2000);
  ESP.restart();
}

void redirectToRoot() {
  server.sendHeader("Location", String("http://") + apIP.toString(), true);
  server.send(302, "text/plain", "");
}

// ----------------------------- Handlers (Control) --------------------------
void handleOn() {
  relaysEnabled = true;
  manualOverride = true;
  alreadyOff = false;

  prefs.begin("settings", false);
  putIfChanged(prefs, "manualOverride", manualOverride, prefs.getBool("manualOverride", false));
  putIfChanged(prefs, "relaysEnabled", relaysEnabled, prefs.getBool("relaysEnabled", false));
  prefs.end();

  server.send(200, "text/plain", "Relays ON (manual override)");
}

void handleOff() {
  relaysEnabled = false;
  manualOverride = true;

  prefs.begin("settings", false);
  putIfChanged(prefs, "manualOverride", manualOverride, prefs.getBool("manualOverride", false));
  putIfChanged(prefs, "relaysEnabled", relaysEnabled, prefs.getBool("relaysEnabled", false));
  prefs.end();

  allOff();
  alreadyOff = true;
  server.send(200, "text/plain", "Relays OFF (manual override)");
}

void handleAuto() {
  manualOverride = false;
  prefs.begin("settings", false);
  putIfChanged(prefs, "manualOverride", manualOverride, prefs.getBool("manualOverride", true));
  prefs.end();
  server.send(200, "text/plain", "Returned to automatic schedule");
}

void handleSetSchedule() {
  String onStr  = server.arg("on");
  String offStr = server.arg("off");
  int h, m;
  bool changed = false;

  prefs.begin("settings", false);

  if (parseTimeStr(onStr, h, m)) {
    changed |= putIfChanged(prefs, "on_h", h, prefs.getInt("on_h", on_h));
    changed |= putIfChanged(prefs, "on_m", m, prefs.getInt("on_m", on_m));
    on_h = h; on_m = m;
  }
  if (parseTimeStr(offStr, h, m)) {
    changed |= putIfChanged(prefs, "off_h", h, prefs.getInt("off_h", off_h));
    changed |= putIfChanged(prefs, "off_m", m, prefs.getInt("off_m", off_m));
    off_h = h; off_m = m;
  }

  prefs.end();
  server.send(200, "text/plain",
    "Schedule updated: ON " + String(on_h) + ":" + String(on_m) +
    "  OFF " + String(off_h) + ":" + String(off_m));
}

void handlePattern() {
  String name = server.arg("name");
  name.toUpperCase();
  Pattern next = currentPattern;

  if (name == "CHASE") next = CHASE;
  else if (name == "WAVE") next = WAVE;
  else if (name == "RANDOM") next = RANDOM;

  bool changed = (next != currentPattern);
  currentPattern = next;

  prefs.begin("settings", false);
  if (changed) putIfChanged(prefs, "pattern", (int)currentPattern, prefs.getInt("pattern", (int)CHASE));
  prefs.end();

  server.send(200, "text/plain", "Pattern set to " + name);
}

void handleSetSpeed() {
  int val = server.arg("val").toInt();
  if (val >= 50 && val <= 1000) {
    bool changed = (val != patternSpeed);
    patternSpeed = val;
    prefs.begin("settings", false);
    if (changed) putIfChanged(prefs, "speed", patternSpeed, prefs.getInt("speed", 200));
    prefs.end();
    server.send(200, "text/plain", "Pattern speed set to " + String(patternSpeed) + " ms");
  } else {
    server.send(200, "text/plain", "Invalid speed value");
  }
}

void handleSunsetMode() {
  bool next = (server.arg("enable") == "1");
  bool changed = (next != useSunset);
  useSunset = next;
  prefs.begin("settings", false);
  if (changed) putIfChanged(prefs, "useSunset", useSunset, !useSunset);
  prefs.end();
  server.send(200, "text/plain", String("Sunset mode: ") + (useSunset ? "ENABLED" : "DISABLED"));
}

void handleSetSunsetOffset() {
  int m = server.arg("min").toInt();
  if (m < -180) m = -180;
  if (m > 180)  m = 180;
  bool changed = (m != sunsetOffsetMin);
  sunsetOffsetMin = m;
  prefs.begin("settings", false);
  if (changed) putIfChanged(prefs, "offset", sunsetOffsetMin, prefs.getInt("offset", 0));
  prefs.end();
  server.send(200, "text/plain", "Sunset offset set to " + String(sunsetOffsetMin) + " minutes");
}

// ----------------------------- Status endpoints ----------------------------
void handleStatusText() {
  struct tm tminfo;
  String msg = "Status:\n";
  if (getLocalTime(&tminfo)) {
    char buf[16]; snprintf(buf, sizeof(buf), "%02d:%02d", tminfo.tm_hour, tminfo.tm_min);
    msg += "Local time: " + String(buf) + "\n";
  } else {
    msg += "Local time: (not set)\n";
  }
  msg += "ON time: " + String(on_h) + ":" + String(on_m) + "\n";
  msg += "OFF time: " + String(off_h) + ":" + String(off_m) + "\n";
  msg += "Pattern: " + String(currentPattern == CHASE ? "CHASE" : currentPattern == WAVE ? "WAVE" : "RANDOM") + "\n";
  msg += "Speed: " + String(patternSpeed) + " ms\n";
  msg += "Sunset mode: " + String(useSunset ? "ON" : "OFF") + "\n";
  msg += "Sunset offset: " + String(sunsetOffsetMin) + " min\n";
  msg += "Relays: " + String(relaysEnabled ? "ENABLED" : "DISABLED") + "\n";
  msg += "Mode: " + String(manualOverride ? "MANUAL" : "AUTO") + "\n";
  server.send(200, "text/plain", msg);
}

void handleStatusJSON() {
  DynamicJsonDocument doc(1024);
  struct tm tminfo;
  if (getLocalTime(&tminfo)) {
    doc["time"]["hour"] = tminfo.tm_hour;
    doc["time"]["min"]  = tminfo.tm_min;
  } else {
    doc["time"] = nullptr;
  }
  doc["on"]["h"] = on_h; doc["on"]["m"] = on_m;
  doc["off"]["h"] = off_h; doc["off"]["m"] = off_m;
  switch (currentPattern) {
    case CHASE:  doc["pattern"] = "CHASE"; break;
    case WAVE:   doc["pattern"] = "WAVE";  break;
    case RANDOM: doc["pattern"] = "RANDOM";break;
  }
  doc["speed_ms"] = patternSpeed;
  doc["sunset"]["enabled"] = useSunset;
  doc["sunset"]["offset_min"] = sunsetOffsetMin;
  doc["relays_enabled"] = relaysEnabled;
  doc["mode"] = manualOverride ? "MANUAL" : "AUTO";
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ----------------------------- Wi-Fi / mDNS / Server -----------------------
void mountRoutes(bool apMode) {
  if (apMode) {
    // Captive portal: redirect everything to /setup
    server.onNotFound({
      if (WiFi.getMode() & WIFI_AP) {
        server.sendHeader("Location", String("http://") + apIP.toString() + "/setup", true);
        server.send(302, "text/plain", "");
      } else {
        server.send(404, "text/plain", "Not found");
      }
    });
  }

  server.on("/", handleRootUI);
  server.on("/setup", handleSetupPage);
  server.on("/save", HTTP_POST, handleSaveSetup);

  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/auto", handleAuto);
  server.on("/status", handleStatusText);
  server.on("/status.json", handleStatusJSON);
  server.on("/setschedule", handleSetSchedule);
  server.on("/pattern", handlePattern);
  server.on("/setspeed", handleSetSpeed);
  server.on("/sunsetmode", handleSunsetMode);
  server.on("/setSunsetOffset", handleSetSunsetOffset);

  server.begin();
}

bool connectWiFiStation() {
  if (wifiSSID.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  Serial.printf("Connecting to %s", wifiSSID.c_str());

  const uint32_t TIMEOUT_MS = 15000;
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi‑Fi connect timeout.");
  return false;
}

void startAPWithPortal() {
  WiFi.mode(WIFI_AP);
  uint32_t chip = (uint32_t)ESP.getEfuseMac();
  char buf[32];
  snprintf(buf, sizeof(buf), "XMAS-SETUP-%04X", (unsigned)(chip & 0xFFFF));
  apSSID = buf;

  WiFi.softAP(apSSID.c_str());
  delay(100);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  // DNS: resolve all domains to AP IP for captive portal experience
  dnsServer.start(DNS_PORT, "*", apIP);

  Serial.print("AP SSID: "); Serial.println(apSSID);
  Serial.print("Open http://"); Serial.println(apIP);

  mountRoutes(true);
}

void startMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS started: http://%s.local/\n", MDNS_NAME);
  } else {
    Serial.println("mDNS start failed.");
  }
}

// ----------------------------- Setup / Loop --------------------------------
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  // Relay pins
  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    setRelay(i, false);
  }

  // Load settings
  prefs.begin("settings", true);
  useSunset        = prefs.getBool("useSunset", true);
  sunsetOffsetMin  = prefs.getInt("offset", 0);
  patternSpeed     = prefs.getInt("speed", 200);
  currentPattern   = (Pattern)prefs.getInt("pattern", (int)CHASE);
  on_h = prefs.getInt("on_h", 18);
  on_m = prefs.getInt("on_m", 0);
  off_h = prefs.getInt("off_h", 22);
  off_m = prefs.getInt("off_m", 0);
  manualOverride   = prefs.getBool("manualOverride", false);
  relaysEnabled    = prefs.getBool("relaysEnabled", false);
  prefs.end();

  // Load Wi-Fi & API key
  prefs.begin("wifi", true);
  wifiSSID = prefs.getString("ssid", "");
  wifiPASS = prefs.getString("pass", "");
  prefs.end();

  prefs.begin("owm", true);
  weatherApiKey = prefs.getString("key", "");
  prefs.end();

  // Configure TZ (DST-aware) and NTP
  setenv("TZ", TZ_RULE, 1);
  tzset();
  configTime(0, 0, ntpServer);

  bool staConnected = connectWiFiStation();
  if (staConnected) {
    startMDNS();
    mountRoutes(false);
    // If we have an API key, fetch sunset right away
    if (weatherApiKey.length()) {
      updateSunsetTime();
      lastSunsetUpdate = millis();
    }
  } else {
    startAPWithPortal(); // captive portal for SSID/PASS/API key
  }
}

void loop() {
  // Web handling
  server.handleClient();
  if (WiFi.getMode() & WIFI_AP) {
    dnsServer.processNextRequest();
  }

  // Periodic sunset update (every 24h) — only in STA mode and if sunset mode enabled
  if ((WiFi.status() == WL_CONNECTED) && useSunset && weatherApiKey.length()) {
    if ((unsigned long)(millis() - lastSunsetUpdate) > 86400000UL) {
      if (updateSunsetTime()) {
        lastSunsetUpdate = millis();
      } else {
        // Try later if it failed
        lastSunsetUpdate = millis() - 86400000UL + 600000UL; // retry in ~10 min
      }
    }
  }

  // Schedule: re-evaluate periodically (1s) when in AUTO
  if (!manualOverride && (unsigned long)(millis() - lastScheduleCheck) > 1000UL) {
    lastScheduleCheck = millis();
    bool shouldBeOn = computeShouldBeOn();
    if (shouldBeOn != relaysEnabled) {
      relaysEnabled = shouldBeOn;
      prefs.begin("settings", false);
      putIfChanged(prefs, "relaysEnabled", relaysEnabled, !relaysEnabled);
      prefs.end();
      alreadyOff = !relaysEnabled ? true : false;
    }
  }

  // Drive patterns (non-blocking)
  if (relaysEnabled) {
    alreadyOff = false;
    stepPatternIfDue();
  } else {
    if (!alreadyOff) { allOff(); alreadyOff = true; }
  }
}
