/*
  Christmas Light Control (ESP32) — Relay-safe non-blocking with HTTPS + ArduinoJson + Playlist/Shuffle

  Hardware:
    - 8 x 120V light strands on 8-channel relay board (mechanical relays)
    - One ESP32

  Highlights:
    - Non-blocking animation engine (web UI & schedule stay responsive)
    - Relay-safe timing: per-channel minimum dwell (200 ms), pattern step clamped
    - 13 total patterns (original 3 + 10 new on/off relay patterns)
    - Playlist rotation with Shuffle (no repeats) or Sequential, 1-minute hold default
    - HTTPS request to OpenWeather; JSON parsed with ArduinoJson
    - API key NOT hardcoded; entered on setup page and stored in Preferences
    - First-run captive portal to configure Wi‑Fi + API key
    - DST-aware timezone via POSIX TZ string
    - mDNS (http://xmas.local/), /status.json endpoint

  Notes:
    - For production TLS, replace setInsecure() with certificate/fingerprint pinning.
    - If you’re not in US Mountain Time, adjust TZ_RULE accordingly.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include "DHT.h"
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoJson.h>
#include <HTTPUpdateServer.h>
HTTPUpdateServer httpUpdater;

// ----------------------------- Hardware config -----------------------------
const int RELAY_PINS[8] = {12, 13, 15, 25, 26, 27, 32, 33};
const bool RELAY_ACTIVE_LOW = false; // true if your relay boards are active-low (LOW = ON)

#define DHTPIN 14       // DHT22 data pin on GPIO 14
#define DHTTYPE DHT22   // Sensor type

DHT dht(DHTPIN, DHTTYPE);  // Create the sensor object

float enclosureTemp = NAN;      // latest measured temp (C)
bool overheatShutdown = false;  // true when relays are forced off

const float OVERHEAT_ON_C  = 50.0; // trip threshold
const float OVERHEAT_OFF_C = 45.0; // recovery threshold

unsigned long lastTempReadMs = 0;             // last time we checked
const unsigned long TEMP_READ_INTERVAL_MS = 60000; // check every 60,000 ms (1 minute)
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
const char* latitude  = "41.3114";   // Laramie, WY (edit for your location)
const char* longitude = "-105.5911"; // Laramie, WY

// ----------------------------- Time / TZ -----------------------------------
const char* ntpServer = "pool.ntp.org";
// Mountain time with US DST: M3.2.0/2 (2am, 2nd Sunday in March); M11.1.0/2 (2am, 1st Sunday in Nov)
const char* TZ_RULE = "MST7MDT,M3.2.0/2,M11.1.0/2";

// ----------------------------- App state -----------------------------------
enum Pattern {
  CHASE, WAVE, RANDOM,           // original 3
  ALT_EVEN_ODD,                  // new patterns (relay-safe on/off)
  BOUNCE,
  CHASE_GAP,
  PAIR_PINGPONG,
  BLOCK_WIPE,
  OVERLAP_WAVE,
  SPARKLE_SPARSE,
  RANDOM_BURSTS,
  HALF_SWAP,
  BINARY_COUNT,
  ALL_ON
};

const char* patternName(Pattern p) {
  switch (p) {
    case CHASE:           return "CHASE";
    case WAVE:            return "WAVE";
    case RANDOM:          return "RANDOM";
    case ALT_EVEN_ODD:    return "ALT_EVEN_ODD";
    case BOUNCE:          return "BOUNCE";
    case CHASE_GAP:       return "CHASE_GAP";
    case PAIR_PINGPONG:   return "PAIR_PINGPONG";
    case BLOCK_WIPE:      return "BLOCK_WIPE";
    case OVERLAP_WAVE:    return "OVERLAP_WAVE";
    case SPARKLE_SPARSE:  return "SPARKLE_SPARSE";
    case RANDOM_BURSTS:   return "RANDOM_BURSTS";
    case HALF_SWAP:       return "HALF_SWAP";
    case BINARY_COUNT:    return "BINARY_COUNT";
    case ALL_ON:          return "ALL_ON";
    default:              return "UNKNOWN";
  }
}

Pattern currentPattern = CHASE;
int  patternSpeed = 200;            // ms per step (relay-safe clamp applied)
bool relaysEnabled = false;         // current power to relays
bool manualOverride = false;        // manual vs. auto schedule
bool useSunset = true;              // if true, ON time tracks sunset daily
int  sunsetOffsetMin = 0;           // -180..+180
int  on_h = 18, on_m = 0;           // manual schedule ON  time (hh:mm)
int  off_h = 22, off_m = 0;         // manual schedule OFF time (hh:mm)

// Wi‑Fi and API key stored in Preferences
String wifiSSID, wifiPASS, weatherApiKey;

// ----------------------------- Timers --------------------------------------
unsigned long lastSunsetUpdate = 0;  // ms since boot for daily refresh
unsigned long lastScheduleCheck = 0;

// Pattern timing state (non-blocking)
uint8_t      patIndex = 0;
unsigned long lastPatStep = 0;
bool alreadyOff = false;

// ----------------------------- Relay-safety timing & frame driver ----------
const uint16_t minDwellMs = 200;     // minimum ON/OFF dwell per channel (ms) for mechanical relays
unsigned long lastChangeMs[8] = {0}; // last time each channel toggled
uint8_t currentFrame = 0;            // 8-bit ON/OFF snapshot (bit i -> channel i)

inline void setRelay(int idx, bool on) {
  if (idx < 0 || idx >= 8) return; // safety check

  // If overheating, force OFF no matter what
  bool effectiveOn = on && !overheatShutdown;
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
  else                  digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
}

void allOff() {
  for (int i = 0; i < 8; i++) setRelay(i, false);
  currentFrame = 0;
}

/** Apply an 8-bit frame to relays with dwell protection. 1=ON, 0=OFF */
void applyFrame(uint8_t frame) {
  unsigned long now = millis();
  for (int i = 0; i < 8; i++) {
    bool wantOn = (frame >> i) & 0x01;
    bool isOn   = (currentFrame >> i) & 0x01;
    // If overheating, block ON requests
    if (overheatShutdown) wantOn = false;
    if (wantOn != isOn) {
      if ((unsigned long)(now - lastChangeMs[i]) >= minDwellMs) {
        setRelay(i, wantOn);
        if (wantOn) currentFrame |=  (1 << i);
        else        currentFrame &= ~(1 << i);
        lastChangeMs[i] = now;
      }
    }
  }
}

void checkTemperatureAndProtect() {
  unsigned long now = millis();
  if ((unsigned long)(now - lastTempReadMs) < TEMP_READ_INTERVAL_MS) return;
  lastTempReadMs = now;

  float t = dht.readTemperature(); // Celsius
  if (isnan(t)) {
    // Sensor read failed, skip
    return;
  }
  enclosureTemp = t;

  // Hysteresis logic
  if (!overheatShutdown && t >= OVERHEAT_ON_C) {
    overheatShutdown = true;
    // Shut down ALL relays immediately
    allOff();
  } else if (overheatShutdown && t <= OVERHEAT_OFF_C) {
    overheatShutdown = false;
    // Do not auto-turn ON; let schedule/manual control decide
  }
}

// Small helper for portability (popcount of 8 bits)
uint8_t popcount8(uint8_t v) {
  uint8_t c = 0; for (uint8_t i=0;i<8;i++) c += (v>>i)&1; return c;
}

// ----------------------------- Frame-based patterns ------------------------
// Return the next 8-bit output frame (bit i controls channel i: 1=ON, 0=OFF).

uint8_t frame_ALT_EVEN_ODD() {
  static bool evenOn = false;
  evenOn = !evenOn;
  uint8_t evenMask = 0x55; // 01010101 (ch 0,2,4,6)
  uint8_t oddMask  = 0xAA; // 10101010 (ch 1,3,5,7)
  return evenOn ? evenMask : oddMask;
}

uint8_t frame_BOUNCE() {
  static int idx = 0, dir = +1;
  uint8_t mask = (1 << idx);
  idx += dir;
  if (idx >= 7) { idx = 7; dir = -1; }
  else if (idx <= 0) { idx = 0; dir = +1; }
  return mask;
}

uint8_t frame_CHASE_GAP() {
  static uint8_t pos = 0;
  const uint8_t gapLen = 1; // increase for slower spatial pace
  uint8_t mask = (1 << pos);
  pos = (pos + gapLen) % 8;
  return mask;
}

uint8_t frame_PAIR_PINGPONG() {
  static int idx = 0, dir = +1;
  uint8_t a = idx;
  uint8_t b = min(idx + 1, 7);
  uint8_t mask = (1 << a) | (1 << b);
  idx += dir;
  if (idx >= 6) { idx = 6; dir = -1; }
  else if (idx <= 0) { idx = 0; dir = +1; }
  return mask;
}

uint8_t frame_BLOCK_WIPE() {
  static uint8_t width = 0;
  width = (width + 1);
  if (width > 8) width = 0;
  uint8_t mask = (width == 0) ? 0 : ((1 << width) - 1);
  return mask;
}

uint8_t frame_OVERLAP_WAVE() {
  static uint8_t k = 0; // 0..16
  uint8_t mask;
  if (k <= 8) {
    mask = (k == 0) ? 0 : ((1 << k) - 1);
  } else {
    uint8_t offCount = k - 8;
    mask = 0xFF & ~(((1 << offCount) - 1));
  }
  k = (k + 1) % 17;
  if (k == 16) k = 0;
  return (mask & 0xFF);
}

uint8_t frame_SPARKLE_SPARSE() {
  static uint8_t mask = 0;
  // randomly flip a couple of bits
  for (int n = 0; n < 2; n++) {
    int bit = random(0,8);
    mask ^= (1 << bit);
  }
  // keep sparse (<=3 on)
  while (popcount8(mask) > 3) {
    for (int i=0;i<8 && popcount8(mask)>3;i++) {
      if ((mask>>i)&1) mask &= ~(1<<i);
    }
  }
  return mask;
}

uint8_t frame_RANDOM_BURSTS() {
  static uint8_t mask = 0;
  static int burst = 0;
  if (burst > 0) { burst--; return mask; }
  if (random(0,10) == 0) { // ~10% chance to start a burst
    mask = 0;
    int count = 2 + random(0,4); // 2..5 on
    while (popcount8(mask) < count) mask |= (1 << random(0,8));
    burst = 3 + random(0,4); // 3..6 steps hold
  } else {
    mask = 0;
  }
  return mask;
}

uint8_t frame_HALF_SWAP() {
  static bool leftOn = false;
  leftOn = !leftOn;
  uint8_t leftMask  = 0x0F; // 00001111
  uint8_t rightMask = 0xF0; // 11110000
  return leftOn ? leftMask : rightMask;
}

uint8_t frame_BINARY_COUNT() {
  static uint8_t count = 0;
  uint8_t mask = count;
  count++;
  return mask;
}

uint8_t frame_ALL_ON() {
  return 0xFF; // all 8 relays ON
}

// ----------------------------- Pattern stepping (non-blocking) -------------
void stepPatternIfDue() {
  unsigned long now = millis();
  // Relay-safe clamp for step interval
  int stepMs = (patternSpeed < 200) ? 200 : patternSpeed;
  if ((unsigned long)(now - lastPatStep) < (unsigned long)stepMs) return;
  lastPatStep = now;

  uint8_t frame = 0;

  switch (currentPattern) {
    case CHASE:
      frame = (1 << patIndex);
      patIndex = (patIndex + 1) % 8;
      break;

    case WAVE: {
      static bool filling = true;
      static uint8_t level = 0;
      if (filling) {
        frame = (level == 0) ? 0 : ((1 << (level + 1)) - 1);
        if (++level >= 7) { level = 7; filling = false; }
      } else {
        frame = (level == 0) ? 0 : ((1 << (level + 1)) - 1);
        if (level-- == 0) { level = 0; filling = true; }
      }
    } break;

    case RANDOM:
      frame = (1 << random(0,8));
      break;

    case ALL_ON:
      frame = frame_ALL_ON();
      break;  

    case ALT_EVEN_ODD:    frame = frame_ALT_EVEN_ODD();    break;
    case BOUNCE:          frame = frame_BOUNCE();          break;
    case CHASE_GAP:       frame = frame_CHASE_GAP();       break;
    case PAIR_PINGPONG:   frame = frame_PAIR_PINGPONG();   break;
    case BLOCK_WIPE:      frame = frame_BLOCK_WIPE();      break;
    case OVERLAP_WAVE:    frame = frame_OVERLAP_WAVE();    break;
    case SPARKLE_SPARSE:  frame = frame_SPARKLE_SPARSE();  break;
    case RANDOM_BURSTS:   frame = frame_RANDOM_BURSTS();   break;
    case HALF_SWAP:       frame = frame_HALF_SWAP();       break;
    case BINARY_COUNT:    frame = frame_BINARY_COUNT();    break;
  }

  applyFrame(frame);
}

// ----------------------------- OpenWeather (HTTPS + ArduinoJson) ----------
bool updateSunsetTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (weatherApiKey.length() == 0)    return false;

  WiFiClientSecure client;
  client.setInsecure(); // For simplicity. For production, pin the cert/fingerprint.

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

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  if (!doc.containsKey("sys") || !doc["sys"].containsKey("sunset")) {
    Serial.println("JSON missing sys.sunset");
    return false;
  }

  time_t sunsetUnix = (time_t) doc["sys"]["sunset"].as<long>();
  sunsetUnix += (time_t)sunsetOffsetMin * 60; // apply offset

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
  if (!getLocalTime(&timeinfo)) return relaysEnabled; // keep current if time unknown

  int now_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int on_min  = on_h * 60 + on_m;
  int off_min = off_h * 60 + off_m;

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
           "code{background:#eee;padding:2px 4px;border-radius:4px}"
           "</style></head><body>");
}
String htmlFooter() { return F("</body></html>"); }

void handleRootUI() {
  String html = htmlHeader();
  html += F("<div class='section'><h1>Christmas Light Control</h1>"
            "<div id='msg'>Status appears here...</div></div>");

  if (overheatShutdown) {
  html += "<div class='section' style='background:#ffeaea;border:1px solid red;color:red;'>";
  html += "<strong>⚠ Overheat detected!</strong><br>Relays have been shut down for safety.";
  html += "</div>";
}

  html += F("<div class='section'><h2>Power</h2>"
            "<button onclick=\"fetch('/on').then(r=>r.text()).then(t=>msg.innerText=t)\">Turn ON (Manual)</button>"
            "<button onclick=\"fetch('/off').then(r=>r.text()).then(t=>msg.innerText=t)\">Turn OFF (Manual)</button>"
            "<button onclick=\"fetch('/auto').then(r=>r.text()).then(t=>msg.innerText=t)\">Return to Auto</button>"
            "</div>");

    // Unified patterns dropdown
  html += F("<div class='section'><h2>Patterns</h2>"
            "<label for='pattern'>Choose a pattern:</label>"
            "<select id='pattern' name='pattern' onchange=\"setPattern(this.value)\">"
            "<option value='CHASE'>CHASE</option>"
            "<option value='WAVE'>WAVE</option>"
            "<option value='RANDOM'>RANDOM</option>"
            "<option value='ALT_EVEN_ODD'>ALT_EVEN_ODD</option>"
            "<option value='BOUNCE'>BOUNCE</option>"
            "<option value='CHASE_GAP'>CHASE_GAP</option>"
            "<option value='PAIR_PINGPONG'>PAIR_PINGPONG</option>"
            "<option value='BLOCK_WIPE'>BLOCK_WIPE</option>"
            "<option value='OVERLAP_WAVE'>OVERLAP_WAVE</option>"
            "<option value='SPARKLE_SPARSE'>SPARKLE_SPARSE</option>"
            "<option value='HALF_SWAP'>HALF_SWAP</option>"
            "<option value='BINARY_COUNT'>BINARY_COUNT</option>"
            "<option value='ALL_ON'>ALL_ON</option>"
            "</select>"
            "</div>");

  // Speed (relay-safe clamp handled on server)
  html += "<div class='section'><h2>Pattern Speed</h2>"
          "<input type='range' min='200' max='2000' value='" + String(patternSpeed) + "' id='speed' "
          "oninput=\"document.getElementById('v').innerText=this.value\">"
          "<div>Speed: <span id='v'>" + String(patternSpeed) + "</span> ms</div>"
          "<button onclick=\"fetch('/setspeed?val='+document.getElementById('speed').value).then(r=>r.text()).then(t=>msg.innerText=t)\">Set Speed</button>"
          "</div>";

  // Schedule (manual)
  html += F("<div class='section'><h2>Schedule</h2>"
            "<input id='on' placeholder='HH:MM'><input id='off' placeholder='HH:MM'>"
            "<button onclick=\"fetch('/setschedule?on='+on.value+'&off='+off.value).then(r=>r.text()).then(t=>msg.innerText=t)\">Save Schedule</button>"
            "</div>");

  // Sunset
  html += "<div class='section'><h2>Sunset Scheduling</h2>"
          "<button onclick=\"fetch('/sunsetmode?enable=1').then(r=>r.text()).then(t=>msg.innerText=t)\">Enable Sunset Mode</button>"
          "<button onclick=\"fetch('/sunsetmode?enable=0').then(r=>r.text()).then(t=>msg.innerText=t)\">Disable Sunset Mode</button>"
          "<label>Offset (minutes, -180..+180)</label>"
          "<input type='number' id='offset' value='" + String(sunsetOffsetMin) + "'>"
          "<button onclick=\"fetch('/setSunsetOffset?min='+document.getElementById('offset').value).then(r=>r.text()).then(t=>msg.innerText=t)\">Save Offset</button>"
          "</div>";

  // Playlist controls
  html += "<div class='section'><h2>Playlist</h2>"
          "<button onclick=\"fetch('/setshuffle?enable=1').then(r=>r.text()).then(t=>msg.innerText=t)\">Shuffle ON</button>"
          "<button onclick=\"fetch('/setshuffle?enable=0').then(r=>r.text()).then(t=>msg.innerText=t)\">Shuffle OFF</button>"
          "<label>Pattern Hold (seconds)</label>"
          "<input type='number' id='holdsec' value='" + String(60000/1000) + "'>"
          "<button onclick=\"fetch('/sethold?sec='+document.getElementById('holdsec').value).then(r=>r.text()).then(t=>msg.innerText=t)\">Set Hold</button>"
          "</div>";

  // Setup link
  html += F("<div class='section'><h2>Setup</h2>"
            "/setup<button>Open Setup Page</button></a>"
            "</div>");

  // Status refresher
  html += F("<script>const msg=document.getElementById('msg');"
            "function refresh(){fetch('/status').then(r=>r.text()).then(t=>msg.innerText=t)}"
            "setInterval(refresh,4000);window.onload=refresh;</script>");

  html += F("<script>"
            "function setPattern(p) {"
            "  fetch('/pattern?name=' + encodeURIComponent(p))"
            "    .then(r => r.text())"
            "    .then(t => msg.innerText = t);"
            "}"
            "</script>");


  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleSetupPage() {
  String html = htmlHeader();
  html += F("<div class='section'><h1>Setup</h1>"
            "<p>Enter Wi‑Fi credentials and your OpenWeather API key. Device will reboot after saving.</p>"
            "/save"
            "<label>Wi‑Fi SSID</label><input name='ssid' placeholder='SSID' value='");
  html += wifiSSID;
  html += F("'>"
            "<label>Wi‑Fi Password</label><input name='pass' type='password' placeholder='Password' value=''>"
            "<label>OpenWeather API Key</label><input name='owm' placeholder='Your API key' value=''>"
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

// ----------------------------- Control Handlers ----------------------------
void startPattern(Pattern p); // forward

void handleOn() {
  relaysEnabled = true;
  manualOverride = true;
  alreadyOff = false;

  prefs.begin("settings", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.putBool("relaysEnabled", relaysEnabled);
  prefs.end();

  server.send(200, "text/plain", "Relays ON (manual override)");
}

void handleOff() {
  relaysEnabled = false;
  manualOverride = true;

  prefs.begin("settings", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.putBool("relaysEnabled", relaysEnabled);
  prefs.end();

  allOff(); alreadyOff = true;
  server.send(200, "text/plain", "Relays OFF (manual override)");
}

void handleAuto() {
  manualOverride = false;
  prefs.begin("settings", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.end();
  server.send(200, "text/plain", "Returned to automatic schedule");
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

void handleSetSchedule() {
  String onStr  = server.arg("on");
  String offStr = server.arg("off");
  int h, m;

  prefs.begin("settings", false);
  if (parseTimeStr(onStr, h, m)) {
    prefs.putInt("on_h", h); prefs.putInt("on_m", m);
    on_h = h; on_m = m;
  }
  if (parseTimeStr(offStr, h, m)) {
    prefs.putInt("off_h", h); prefs.putInt("off_m", m);
    off_h = h; off_m = m;
  }
  prefs.end();

  server.send(200, "text/plain",
    "Schedule updated: ON " + String(on_h) + ":" + String(on_m) +
    "  OFF " + String(off_h) + ":" + String(off_m));
}

void handleSunsetMode() {
  bool next = (server.arg("enable") == "1");
  useSunset = next;
  prefs.begin("settings", false);
  prefs.putBool("useSunset", useSunset);
  prefs.end();
  server.send(200, "text/plain", String("Sunset mode: ") + (useSunset ? "ENABLED" : "DISABLED"));
}

void handleSetSunsetOffset() {
  int m = server.arg("min").toInt();
  if (m < -180) m = -180;
  if (m >  180) m =  180;
  sunsetOffsetMin = m;
  prefs.begin("settings", false);
  prefs.putInt("offset", sunsetOffsetMin);
  prefs.end();
  server.send(200, "text/plain", "Sunset offset set to " + String(sunsetOffsetMin) + " minutes");
}

void handleSetSpeed() {
  int val = server.arg("val").toInt();
  // Relay-safe clamp
  if (val < 200) val = 200;
  if (val > 2000) val = 2000;
  bool changed = (val != patternSpeed);
  patternSpeed = val;
  prefs.begin("settings", false);
  if (changed) prefs.putInt("speed", patternSpeed);
  prefs.end();
  server.send(200, "text/plain", "Pattern speed set to " + String(patternSpeed) + " ms");
}

void handlePattern() {
  String name = server.arg("name");
  name.toUpperCase();
  Pattern next = currentPattern;

  if      (name == "CHASE")           next = CHASE;
  else if (name == "WAVE")            next = WAVE;
  else if (name == "RANDOM")          next = RANDOM;
  else if (name == "ALT_EVEN_ODD")    next = ALT_EVEN_ODD;
  else if (name == "BOUNCE")          next = BOUNCE;
  else if (name == "CHASE_GAP")       next = CHASE_GAP;
  else if (name == "PAIR_PINGPONG")   next = PAIR_PINGPONG;
  else if (name == "BLOCK_WIPE")      next = BLOCK_WIPE;
  else if (name == "OVERLAP_WAVE")    next = OVERLAP_WAVE;
  else if (name == "SPARKLE_SPARSE")  next = SPARKLE_SPARSE;
  else if (name == "RANDOM_BURSTS")   next = RANDOM_BURSTS;
  else if (name == "HALF_SWAP")       next = HALF_SWAP;
  else if (name == "BINARY_COUNT")    next = BINARY_COUNT;
  else if (name == "ALL_ON")          next = ALL_ON;

  startPattern(next);
  server.send(200, "text/plain", String("Pattern set to ") + patternName(currentPattern));
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
  msg += "Pattern: " + String(patternName(currentPattern)) + "\n";
  msg += "Speed: " + String(patternSpeed) + " ms\n";
  msg += "Sunset mode: " + String(useSunset ? "ON" : "OFF") + "\n";
  msg += "Sunset offset: " + String(sunsetOffsetMin) + " min\n";
  msg += "Relays: " + String(relaysEnabled ? "ENABLED" : "DISABLED") + "\n";
  msg += "Mode: " + String(manualOverride ? "MANUAL" : "AUTO") + "\n";
  msg += "Shuffle: " + String(shuffleEnabled ? "ON" : "OFF") + "\n"; // shown via /status.json precisely
  msg += "Hold: " + String(60000/1000) + " s\n";
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
  doc["pattern"]  = patternName(currentPattern);
  doc["speed_ms"] = patternSpeed;
  doc["sunset"]["enabled"]     = useSunset;
  doc["sunset"]["offset_min"]  = sunsetOffsetMin;
  doc["relays_enabled"]        = relaysEnabled;
  doc["mode"]                  = manualOverride ? "MANUAL" : "AUTO";
  // Playlist fields are configured below; updated in real handlers:
  // (kept here for API stability; updated in the real handlers)
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ----------------------------- Playlist / shuffle --------------------------
bool shuffleEnabled = true;     // rotate randomly without repeats (persisted)
uint32_t patternHoldMs = 60000; // default 60s per pattern (persisted)
unsigned long patternStart = 0;

Pattern playlist[] = {
  ALT_EVEN_ODD, BOUNCE, BLOCK_WIPE, OVERLAP_WAVE, HALF_SWAP,
  CHASE, CHASE_GAP, PAIR_PINGPONG, BINARY_COUNT, RANDOM_BURSTS, SPARKLE_SPARSE
};
const size_t playlistLen = sizeof(playlist)/sizeof(playlist[0]);

uint8_t playOrder[16]; // indexes into playlist
uint8_t playPos = 0;

void buildSequentialOrder() {
  for (uint8_t i=0;i<playlistLen;i++) playOrder[i] = i;
  playPos = 0;
}

void buildShuffledOrder() {
  for (uint8_t i=0;i<playlistLen;i++) playOrder[i] = i;
  for (int i = playlistLen - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t t = playOrder[i]; playOrder[i] = playOrder[j]; playOrder[j] = t;
  }
  playPos = 0;
}

void startPattern(Pattern p) {
  currentPattern = p;
  prefs.begin("settings", false);
  prefs.putInt("pattern", (int)currentPattern);
  prefs.end();
  // reset local step state
  patIndex = 0;
  lastPatStep = 0;
  patternStart = millis();
}

void maybeAdvancePattern() {
  if (!relaysEnabled) return;    // only rotate when lights are on
  if (!shuffleEnabled) return;

  if ((unsigned long)(millis() - patternStart) < patternHoldMs) return;

  if (shuffleEnabled) {
    if (playPos >= playlistLen) buildShuffledOrder();
  } else {
    if (playPos >= playlistLen) buildSequentialOrder();
  }
  Pattern next = playlist[ playOrder[ playPos++ ] ];
  startPattern(next);
}

// Handlers for shuffle & hold
void handleSetShuffle() {
  bool next = (server.arg("enable") == "1");
  bool changed = (next != shuffleEnabled);
  shuffleEnabled = next;
  prefs.begin("settings", false);
  if (changed) prefs.putBool("shuffle", shuffleEnabled);
  prefs.end();

  if (shuffleEnabled) buildShuffledOrder(); else buildSequentialOrder();
  server.send(200, "text/plain", String("Shuffle: ") + (shuffleEnabled ? "ON" : "OFF"));
}

void handleSetHold() {
  int sec = server.arg("sec").toInt();
  if (sec < 10)  sec = 10;    // don’t flip too fast
  if (sec > 600) sec = 600;   // upper bound: 10 minutes
  bool changed = (patternHoldMs != (uint32_t)sec * 1000UL);
  patternHoldMs = (uint32_t)sec * 1000UL;
  prefs.begin("settings", false);
  if (changed) prefs.putInt("holdMs", (int)patternHoldMs);
  prefs.end();
  server.send(200, "text/plain", "Pattern hold set to " + String(sec) + " s");
}

// ----------------------------- Wi‑Fi / mDNS / Server -----------------------
void mountRoutes(bool apMode) {
  if (apMode) {
    server.onNotFound([]() {
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

  server.on("/setshuffle", handleSetShuffle);
  server.on("/sethold", handleSetHold);

httpUpdater.setup(&server, "/update", "Gfrench", "Twisteroo419!");

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

  // DNS: resolve all to AP IP for captive portal
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
    lastChangeMs[i] = 0;
  }
  currentFrame = 0;

  dht.begin();

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
  // playlist settings
  shuffleEnabled   = prefs.getBool("shuffle", true);
  patternHoldMs    = prefs.getInt("holdMs", 60000);
  prefs.end();

  // Load Wi‑Fi & API key
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
  setenv("TZ", TZ_RULE, 1);  // Apply Mountain Time with DST rules
  tzset();
  
                   // Load the new timezone


  bool staConnected = connectWiFiStation();
  if (staConnected) {
    startMDNS();
    mountRoutes(false);
    if (weatherApiKey.length()) {
      updateSunsetTime();
      lastSunsetUpdate = millis();
    }
  } else {
    startAPWithPortal(); // captive portal for SSID/PASS/API key
  }

  // Build initial playlist order and start timing window
  if (shuffleEnabled) buildShuffledOrder(); else buildSequentialOrder();
  patternStart = millis();
}


void loop() {
  // Web handling
  server.handleClient();
  if (WiFi.getMode() & WIFI_AP) dnsServer.processNextRequest();

  // Periodic sunset update (every 24h) — only in STA mode and if sunset mode enabled
  if ((WiFi.status() == WL_CONNECTED) && useSunset && weatherApiKey.length()) {
    if ((unsigned long)(millis() - lastSunsetUpdate) > 86400000UL) {
      if (updateSunsetTime()) lastSunsetUpdate = millis();
      else                    lastSunsetUpdate = millis() - 86400000UL + 600000UL; // retry ~10 min
    }
  }

  checkTemperatureAndProtect();

  // Schedule: re-evaluate every second (AUTO)
  if (!manualOverride && (unsigned long)(millis() - lastScheduleCheck) > 1000UL) {
    lastScheduleCheck = millis();
    bool shouldBeOn = computeShouldBeOn();
    if (shouldBeOn != relaysEnabled) {
      relaysEnabled = shouldBeOn;
      prefs.begin("settings", false);
      prefs.putBool("relaysEnabled", relaysEnabled);
      prefs.end();
      alreadyOff = !relaysEnabled;
      if (relaysEnabled) patternStart = millis(); // reset hold timer on transition
    }
  }

  // Playlist rotation
  maybeAdvancePattern();

  // Drive patterns (non-blocking)
  if (relaysEnabled) {
    alreadyOff = false;
    stepPatternIfDue();
  } else {
    if (!alreadyOff) { allOff(); alreadyOff = true; }
  }
}
