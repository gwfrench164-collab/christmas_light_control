/*
  Christmas Light Control (ESP32) — Final consolidated sketch
  - OTA-from-UI fields in Setup page
  - All-ON mode (holds all relays ON until toggled off)
  - Shuffle OFF pauses automatic rotation (stay on current pattern)
  - Safe relay API and per-channel dwell protection
  - Unified "xmas" preferences namespace
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
#include <HTTPUpdateServer.h>
#include "DHT.h"

HTTPUpdateServer httpUpdater;
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

#define DHTPIN 14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ----------------------------- Hardware config -----------------------------
const int RELAY_PINS[8] = {12, 13, 15, 25, 26, 27, 32, 33};
const bool RELAY_ACTIVE_LOW = false; // set true if your relay board is active-low

// ----------------------------- Safety & state -------------------------------
const float OVERHEAT_ON_C  = 50.0;
const float OVERHEAT_OFF_C = 45.0;

float enclosureTemp = NAN;
bool overheatShutdown = false;

const uint16_t minDwellMs = 200; // per-channel minimum dwell (ms)
unsigned long lastChangeMs[8];

uint8_t currentFrame = 0; // bit per channel (1=ON)
bool relaysEnabled = false;   // overall power allowed
bool manualOverride = false;   // manual vs auto schedule
bool alreadyOff = false;       // whether we've already forced all off

// New: All-ON mode (holds relays all ON and pauses patterns)
bool allOnMode = false;

// ----------------------------- Timing / patterns ---------------------------
enum Pattern {
  CHASE, WAVE, RANDOM,
  ALT_EVEN_ODD, BOUNCE, CHASE_GAP, PAIR_PINGPONG,
  BLOCK_WIPE, OVERLAP_WAVE, SPARKLE_SPARSE, RANDOM_BURSTS,
  HALF_SWAP, BINARY_COUNT
};

const char* patternName(Pattern p) {
  switch(p) {
    case CHASE: return "CHASE";
    case WAVE: return "WAVE";
    case RANDOM: return "RANDOM";
    case ALT_EVEN_ODD: return "ALT_EVEN_ODD";
    case BOUNCE: return "BOUNCE";
    case CHASE_GAP: return "CHASE_GAP";
    case PAIR_PINGPONG: return "PAIR_PINGPONG";
    case BLOCK_WIPE: return "BLOCK_WIPE";
    case OVERLAP_WAVE: return "OVERLAP_WAVE";
    case SPARKLE_SPARSE: return "SPARKLE_SPARSE";
    case RANDOM_BURSTS: return "RANDOM_BURSTS";
    case HALF_SWAP: return "HALF_SWAP";
    case BINARY_COUNT: return "BINARY_COUNT";
    default: return "UNKNOWN";
  }
}

Pattern currentPattern = CHASE;
int patternSpeed = 200; // ms per step (clamped to minDwell)
uint8_t patIndex = 0;
unsigned long lastPatStep = 0;

// playlist/rotation
bool shuffleEnabled = true; // true => automatic rotation allowed (shuffle)
uint32_t patternHoldMs = 60000; // default 60s
unsigned long patternStart = 0;

Pattern playlist[] = {
  ALT_EVEN_ODD, BOUNCE, BLOCK_WIPE, OVERLAP_WAVE, HALF_SWAP,
  CHASE, CHASE_GAP, PAIR_PINGPONG, BINARY_COUNT, RANDOM_BURSTS, SPARKLE_SPARSE
};
const size_t playlistLen = sizeof(playlist) / sizeof(playlist[0]);

uint8_t playOrder[16];
uint8_t playPos = 0;

// ----------------------------- WiFi / OpenWeather / TZ ---------------------
const char* ntpServer = "pool.ntp.org";
const char* TZ_RULE = "MST7MDT,M3.2.0/2,M11.1.0/2"; // adjust for your TZ if needed
const char* MDNS_NAME = "xmas";

String wifiSSID, wifiPASS, weatherApiKey;
const char* latitude  = "41.3114";
const char* longitude = "-105.5911";

unsigned long lastSunsetUpdate = 0;
int sunsetOffsetMin = 0;
bool useSunset = true;
int on_h = 18, on_m = 0;
int off_h = 22, off_m = 0;

// status/timers
unsigned long lastTempReadMs = 0;
const unsigned long TEMP_READ_INTERVAL_MS = 60000;
unsigned long lastScheduleCheck = 0;

// Reboot control
volatile bool rebootPending = false;
unsigned long rebootArmedAt = 0;
const unsigned long REBOOT_DELAY_MS = 1500;

// ----------------------------- Helper functions ----------------------------

// HTML escape small helper
String htmlEscape(const String &s) {
  String out; out.reserve(s.length());
  for (size_t i=0;i<s.length();i++) {
    char c = s[i];
    switch(c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// Low-level direct write to relay pin
inline void _writeRelayDirect(int idx, bool on) {
  if (idx < 0 || idx >= 8) return;
  int pin = RELAY_PINS[idx];
  if (RELAY_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
  else digitalWrite(pin, on ? HIGH : LOW);
}

// Initialize relay pins (call in setup)
void initRelaysPins() {
  unsigned long now = millis();
  for (int i = 0; i < 8; ++i) {
    pinMode(RELAY_PINS[i], OUTPUT);
    _writeRelayDirect(i, false);
    lastChangeMs[i] = (now >= minDwellMs) ? (now - minDwellMs) : 0;
  }
  currentFrame = 0;
  relaysEnabled = false;
  manualOverride = false;
  alreadyOff = true;
  allOnMode = false;
}

// Safe per-channel set (enforces overheat and min-dwell and global relaysEnabled)
int setRelaySafe(int idx, bool wantOn) {
  if (idx < 0 || idx >= 8) return -2;
  // If overheat, force OFF
  if (overheatShutdown) wantOn = false;
  // If global relays are disabled, we do not allow turning on
  if (!relaysEnabled) wantOn = false;

  bool isOn = ((currentFrame >> idx) & 0x01);
  if (isOn == wantOn) return 0;

  unsigned long now = millis();
  if ((unsigned long)(now - lastChangeMs[idx]) < (unsigned long)minDwellMs) {
    return -1; // too soon
  }

  _writeRelayDirect(idx, wantOn);
  lastChangeMs[idx] = now;

  if (wantOn) currentFrame |= (1 << idx);
  else currentFrame &= ~(1 << idx);

  return 1;
}

// Force all relays OFF immediately (and update lastChangeMs)
void forceAllRelaysOff() {
  unsigned long now = millis();
  for (int i = 0; i < 8; ++i) {
    _writeRelayDirect(i, false);
    currentFrame &= ~(1 << i);
    lastChangeMs[i] = now;
  }
}

// Apply an 8-bit frame to relays using safe setter
void applyFrame(uint8_t frame) {
  for (int i = 0; i < 8; ++i) {
    bool wantOn = (frame >> i) & 0x01;
    setRelaySafe(i, wantOn);
  }
}

// ----------------------------- Temperature protection ---------------------
void checkTemperatureAndProtect() {
  unsigned long now = millis();
  if ((unsigned long)(now - lastTempReadMs) < TEMP_READ_INTERVAL_MS) return;
  lastTempReadMs = now;

  float t = dht.readTemperature();
  if (isnan(t)) return;
  enclosureTemp = t;

  if (!overheatShutdown && enclosureTemp >= OVERHEAT_ON_C) {
    overheatShutdown = true;
    Serial.printf("Overheat: %.1f°C -> forcing ALL OFF\n", enclosureTemp);
    forceAllRelaysOff();
    relaysEnabled = false;
  } else if (overheatShutdown && enclosureTemp <= OVERHEAT_OFF_C) {
    overheatShutdown = false;
    Serial.printf("Temperature recovered: %.1f°C -> overheat cleared\n", enclosureTemp);
    // Do not auto-enable relays
  }
}

// ----------------------------- Pattern frames ------------------------------
inline uint8_t popcount8(uint8_t v) { return (uint8_t)__builtin_popcount((unsigned)v); }

uint8_t frame_ALT_EVEN_ODD() {
  static bool evenOn = false;
  evenOn = !evenOn;
  return evenOn ? 0x55 : 0xAA;
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
  const uint8_t gapLen = 1;
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
  width = (width + 1) % 9; // 0..8
  return (width == 0) ? 0 : ((1 << width) - 1);
}
uint8_t frame_OVERLAP_WAVE() {
  static uint8_t k = 0; // 0..16
  uint8_t mask;
  if (k <= 8) mask = (k == 0) ? 0 : ((1 << k) - 1);
  else {
    uint8_t offCount = k - 8;
    mask = 0xFF & ~(((1 << offCount) - 1));
  }
  k = (k + 1) % 17;
  return mask;
}
uint8_t frame_SPARKLE_SPARSE() {
  static uint8_t mask = 0;
  for (int n = 0; n < 2; n++) {
    int bit = random(0,8);
    mask ^= (1 << bit);
  }
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
  if (random(0,10) == 0) {
    mask = 0;
    int count = 2 + random(0,4);
    while (popcount8(mask) < count) mask |= (1 << random(0,8));
    burst = 3 + random(0,4);
  } else {
    mask = 0;
  }
  return mask;
}
uint8_t frame_HALF_SWAP() {
  static bool leftOn = false;
  leftOn = !leftOn;
  return leftOn ? 0x0F : 0xF0;
}
uint8_t frame_BINARY_COUNT() {
  static uint8_t count = 0;
  uint8_t mask = count;
  count++;
  return mask;
}

// ----------------------------- Stepper (non-blocking) ----------------------
void stepPatternIfDue() {
  if (allOnMode) return; // don't run patterns while All-ON active

  unsigned long now = millis();
  int stepMs = (patternSpeed < (int)minDwellMs) ? minDwellMs : patternSpeed;
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
    case ALT_EVEN_ODD: frame = frame_ALT_EVEN_ODD(); break;
    case BOUNCE: frame = frame_BOUNCE(); break;
    case CHASE_GAP: frame = frame_CHASE_GAP(); break;
    case PAIR_PINGPONG: frame = frame_PAIR_PINGPONG(); break;
    case BLOCK_WIPE: frame = frame_BLOCK_WIPE(); break;
    case OVERLAP_WAVE: frame = frame_OVERLAP_WAVE(); break;
    case SPARKLE_SPARSE: frame = frame_SPARKLE_SPARSE(); break;
    case RANDOM_BURSTS: frame = frame_RANDOM_BURSTS(); break;
    case HALF_SWAP: frame = frame_HALF_SWAP(); break;
    case BINARY_COUNT: frame = frame_BINARY_COUNT(); break;
    default: frame = 0; break;
  }

  applyFrame(frame);
}

// ----------------------------- Playlist / rotation ------------------------
void buildSequentialOrder() {
  for (uint8_t i=0;i<playlistLen;i++) playOrder[i] = i;
  playPos = 0;
}
void buildShuffledOrder() {
  for (uint8_t i=0;i<playlistLen;i++) playOrder[i] = i;
  for (int i = (int)playlistLen - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t t = playOrder[i]; playOrder[i] = playOrder[j]; playOrder[j] = t;
  }
  playPos = 0;
}

void startPattern(Pattern p) {
  currentPattern = p;
  prefs.begin("xmas", false);
  prefs.putInt("pattern", (int)currentPattern);
  prefs.end();
  patIndex = 0;
  lastPatStep = 0;
  patternStart = millis();
}

// Only auto-advance when shuffleEnabled is true and not in allOnMode
void maybeAdvancePattern() {
  if (!relaysEnabled) return;
  if (allOnMode) return;         // pause rotation while All-ON active
  if (!shuffleEnabled) return;   // when shuffle is OFF we pause rotation

  if ((unsigned long)(millis() - patternStart) < patternHoldMs) return;

  if (playPos >= playlistLen) buildShuffledOrder();
  Pattern next = playlist[ playOrder[ playPos++ ] ];
  startPattern(next);
}

// ----------------------------- OpenWeather (sunset) -----------------------
bool updateSunsetTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (weatherApiKey.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure(); // dev-only; replace with cert pinning for production

  HTTPClient http;
  String url = "https://api.openweathermap.org/data/2.5/weather?lat=" +
               String(latitude) + "&lon=" + String(longitude) +
               "&appid=" + weatherApiKey;

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("Weather API error: %d\n", httpCode);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.print("JSON parse error: "); Serial.println(err.c_str());
    return false;
  }

  if (!doc.containsKey("sys") || !doc["sys"].containsKey("sunset")) {
    Serial.println("JSON missing sys.sunset");
    return false;
  }

  uint32_t sunsetEpoch = doc["sys"]["sunset"].as<uint32_t>();
  int32_t offsetSec = (int32_t)sunsetOffsetMin * 60;
  time_t sunsetUnix = (time_t)(sunsetEpoch + offsetSec);

  struct tm tmbuf; struct tm *tminfo = nullptr;
  #ifdef __GNUC__
    if (localtime_r(&sunsetUnix, &tmbuf) != NULL) tminfo = &tmbuf;
  #endif
  if (!tminfo) tminfo = localtime(&sunsetUnix);
  if (!tminfo) {
    Serial.println("localtime() failed for sunset");
    return false;
  }

  on_h = tminfo->tm_hour; on_m = tminfo->tm_min;
  lastSunsetUpdate = millis();
  Serial.printf("Updated ON to sunset%+d: %02d:%02d\n", sunsetOffsetMin, on_h, on_m);
  return true;
}

// ----------------------------- Scheduling ---------------------------------
bool computeShouldBeOn() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return relaysEnabled;
  int now_min = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int on_min = on_h * 60 + on_m;
  int off_min = off_h * 60 + off_m;
  if (on_min <= off_min) return (now_min >= on_min && now_min < off_min);
  else return (now_min >= on_min || now_min < off_min);
}

// ----------------------------- Web UI / endpoints -------------------------

String htmlHeader() {
  return F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<style>body{font-family:Arial;margin:10px;background:#f9f9f9}.section{background:#fff;padding:12px;margin-bottom:12px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.08)}"
           "h1,h2{font-size:1.1em;margin:0 0 8px 0}button{display:block;width:100%;padding:10px;margin:6px 0;border:none;border-radius:6px;background:#0078d7;color:#fff}input,select{width:100%;padding:8px;margin:6px 0;border:1px solid #ccc;border-radius:6px}#msg{white-space:pre-line;border:1px solid #ccc;padding:10px;border-radius:6px;background:#fff}</style></head><body>");
}
String htmlFooter() { return F("</body></html>"); }

void handleRootUI() {
  String html = htmlHeader();
  html += F("<div class='section'><h1>Christmas Light Control</h1><div id='msg'>Status appears here...</div></div>");

  if (overheatShutdown) {
    html += "<div class='section' style='background:#ffeaea;border:1px solid red;color:red;'><strong>⚠ Overheat detected!</strong><br>Relays shut down.</div>";
  }

  // Power
  html += F("<div class='section'><h2>Power</h2>"
            "<button id='btnOn'>Turn ON (Manual)</button>"
            "<button id='btnOff'>Turn OFF (Manual)</button>"
            "<button id='btnAuto'>Return to Auto</button>"
            "</div>");

  // Patterns (includes All ON button — not part of playlist)
  html += F("<div class='section'><h2>Patterns</h2>"
            "<label for='pattern'>Choose a pattern:</label>"
            "<select id='pattern' name='pattern'>"
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
            "<option value='RANDOM_BURSTS'>RANDOM_BURSTS</option>"
            "<option value='HALF_SWAP'>HALF_SWAP</option>"
            "<option value='BINARY_COUNT'>BINARY_COUNT</option>"
            "</select>"
            "<button id='btnSetPattern'>Set Pattern</button>"
            "<button id='btnAllOn'>Turn ALL ON (toggle)</button>"
            "</div>");

  // Speed
  html += "<div class='section'><h2>Pattern Speed</h2>"
          "<input type='range' min='200' max='2000' value='" + String(patternSpeed) + "' id='speed' oninput=\"document.getElementById('v').innerText=this.value\">"
          "<div>Speed: <span id='v'>" + String(patternSpeed) + "</span> ms</div>"
          "<button id='btnSetSpeed'>Set Speed</button>"
          "</div>";

  // Schedule
  html += F("<div class='section'><h2>Schedule</h2>"
            "<input id='on' placeholder='HH:MM'><input id='off' placeholder='HH:MM'>"
            "<button id='btnSaveSchedule'>Save Schedule</button>"
            "</div>");

  // Sunset
  html += "<div class='section'><h2>Sunset Scheduling</h2>"
          "<button id='btnEnableSun'>Enable Sunset Mode</button>"
          "<button id='btnDisableSun'>Disable Sunset Mode</button>"
          "<label>Offset (minutes, -180..+180)</label>"
          "<input type='number' id='offset' value='" + String(sunsetOffsetMin) + "'>"
          "<button id='btnSaveOffset'>Save Offset</button>"
          "</div>";

  // Playlist controls
  html += "<div class='section'><h2>Playlist</h2>"
          "<button id='btnShuffleOn'>Shuffle ON</button>"
          "<button id='btnShuffleOff'>Shuffle OFF (Pause Rotation)</button>"
          "<label>Pattern Hold (seconds)</label>"
          "<input type='number' id='holdsec' value='" + String(patternHoldMs / 1000UL) + "'>"
          "<button id='btnSetHold'>Set Hold</button>"
          "</div>";

  // Setup link
  html += F("<div class='section'><h2>Setup</h2><a href='/setup'><button type='button'>Open Setup Page</button></a></div>");

  // script
  html += F("<script>"
            "const msg = document.getElementById('msg');"
            "function refresh(){ fetch('/status.json?ts=' + Date.now()).then(r=>r.json()).then(j=>{"
            " msg.innerText = 'State: ' + (j.relays_enabled ? 'ON' : 'OFF') + '\\nPattern: ' + j.pattern + '\\nShuffle: ' + (j.shuffle ? 'ON' : 'OFF') + '\\nAll-ON: ' + (j.all_on ? 'ON' : 'OFF');"
            " document.getElementById('speed').value = j.speed_ms; document.getElementById('v').innerText = j.speed_ms;"
            " document.getElementById('pattern').value = j.pattern;"
            " document.getElementById('offset').value = j.sunset.offset_min;"
            "}).catch(()=>{}); }"
            "setInterval(refresh,5000); window.addEventListener('load', refresh);"

            "document.getElementById('btnOn').onclick = ()=>fetch('/on').then(r=>r.text()).then(t=>msg.innerText=t);"
            "document.getElementById('btnOff').onclick = ()=>fetch('/off').then(r=>r.text()).then(t=>msg.innerText=t);"
            "document.getElementById('btnAuto').onclick = ()=>fetch('/auto').then(r=>r.text()).then(t=>msg.innerText=t);"

            "document.getElementById('btnSetPattern').onclick = function(){"
            "  const p = document.getElementById('pattern').value;"
            "  fetch('/setpattern?name='+encodeURIComponent(p)).then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnAllOn').onclick = function(){"
            "  fetch('/allon').then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnSetSpeed').onclick = function(){"
            "  const v = document.getElementById('speed').value;"
            "  fetch('/setspeed?val='+encodeURIComponent(v)).then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnSaveSchedule').onclick = function(){"
            "  const onv = document.getElementById('on').value; const offv = document.getElementById('off').value;"
            "  fetch('/setschedule?on='+encodeURIComponent(onv)+'&off='+encodeURIComponent(offv)).then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnEnableSun').onclick = ()=>fetch('/sunsetmode?enable=1').then(r=>r.text()).then(t=>msg.innerText=t);"
            "document.getElementById('btnDisableSun').onclick = ()=>fetch('/sunsetmode?enable=0').then(r=>r.text()).then(t=>msg.innerText=t);"
            "document.getElementById('btnSaveOffset').onclick = function(){"
            "  const v = document.getElementById('offset').value;"
            "  fetch('/setSunsetOffset?min='+encodeURIComponent(v)).then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnShuffleOn').onclick = function(){"
            "  fetch('/setshuffle?enable=1').then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"
            "document.getElementById('btnShuffleOff').onclick = function(){"
            "  fetch('/setshuffle?enable=0').then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "document.getElementById('btnSetHold').onclick = function(){"
            "  const s = document.getElementById('holdsec').value;"
            "  fetch('/sethold?sec='+encodeURIComponent(s)).then(r=>r.text()).then(t=>msg.innerText=t);"
            "};"

            "</script>");

  html += htmlFooter();
  server.send(200, "text/html", html);
}

// ----------------------------- Setup page with OTA fields ------------------
void handleSetupPage() {
  // Read current saved OTA username for display (do not show password)
  prefs.begin("xmas", true);
  String curOtaUser = prefs.getString("ota_user", "");
  prefs.end();

  String html = htmlHeader();
  html += F("<div class='section'><h1>Setup</h1><p>Enter Wi-Fi, OpenWeather API key, and (optionally) OTA credentials. Save to store.</p><form method='POST' action='/save'>");

  html += F("<label>Wi-Fi SSID</label><input name='ssid' placeholder='SSID' value='");
  html += htmlEscape(wifiSSID);
  html += F("'>");

  html += F("<label>Wi-Fi Password</label><input name='pass' type='password' placeholder='Password' value=''>");

  html += F("<label>OpenWeather API Key</label><input name='owm' placeholder='Your API key' value=''>");

  // OTA fields
  html += F("<h3>OTA (Optional)</h3>"
            "<label>OTA Username</label><input name='ota_user' placeholder='ota user' value='");
  html += htmlEscape(curOtaUser);
  html += F("'>"
            "<label>OTA Password</label><input name='ota_pass' type='password' placeholder='(hidden)' value=''>"
            "<p style='font-size:0.9em;color:#666'>Note: OTA will only be enabled while the device is connected to your Wi-Fi (station mode). Do not enable on public networks.</p>");

  html += F("<button type='submit'>Save & Reboot</button></form></div>");
  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleSaveSetup() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String owm  = server.arg("owm");
  String otaUser = server.arg("ota_user");
  String otaPass = server.arg("ota_pass");

  prefs.begin("xmas", false);
  if (ssid.length()) prefs.putString("wifi_ssid", ssid);
  if (pass.length()) prefs.putString("wifi_pass", pass);
  if (owm.length())  prefs.putString("weather_key", owm);

  // Save OTA creds only if both provided (empty clears)
  if (otaUser.length() && otaPass.length()) {
    prefs.putString("ota_user", otaUser);
    prefs.putString("ota_pass", otaPass);
  } else if (!otaUser.length() && !otaPass.length()) {
    prefs.remove("ota_user");
    prefs.remove("ota_pass");
  }
  prefs.end();

  // If currently connected to Wi-Fi and creds exist, enable OTA now
  prefs.begin("xmas", true);
  String savedUser = prefs.getString("ota_user", "");
  String savedPass = prefs.getString("ota_pass", "");
  prefs.end();

  if (WiFi.status() == WL_CONNECTED && savedUser.length() > 0 && savedPass.length() > 0) {
    httpUpdater.setup(&server, "/update", savedUser.c_str(), savedPass.c_str());
    Serial.println("OTA enabled immediately after save.");
  } else {
    Serial.println("OTA saved to prefs; will enable when device is in station mode on next boot.");
  }

  server.send(200, "text/plain", "Saved. Rebooting shortly...");
  rebootPending = true;
  rebootArmedAt = millis();
}

// ----------------------------- Control handlers ----------------------------
void startPattern(Pattern p); // forward

void handleOn() {
  // Cancel allOnMode? Keep it - user pressed manual ON: we keep allOnMode unchanged.
  relaysEnabled = true;
  manualOverride = true;
  alreadyOff = false;

  prefs.begin("xmas", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.putBool("relaysEnabled", relaysEnabled);
  prefs.end();

  server.send(200, "text/plain", "Relays ON (manual override)");
}

void handleOff() {
  // Turning off should cancel All-ON mode (we don't want it to keep trying)
  allOnMode = false;

  relaysEnabled = false;
  manualOverride = true;

  prefs.begin("xmas", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.putBool("relaysEnabled", relaysEnabled);
  prefs.end();

  forceAllRelaysOff();
  alreadyOff = true;
  server.send(200, "text/plain", "Relays OFF (manual override)");
}

void handleAuto() {
  // Optionally: cancel allOnMode when returning to Auto? We leave it as-is so All-ON can persist
  // If you want All-ON to be manual-only (cancel on Auto), set allOnMode=false here.
  manualOverride = false;
  prefs.begin("xmas", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.end();
  server.send(200, "text/plain", "Returned to automatic schedule");
}

// All-ON toggle handler (under Patterns). Not part of playlist. Respects overheat.
void handleAllOn() {
  if (allOnMode) {
    // turn it off - resume normal behavior (do not change relaysEnabled)
    allOnMode = false;
    patternStart = millis(); // avoid immediate auto-advance
    server.send(200, "text/plain", "All-ON mode cancelled. Resuming pattern behavior.");
    Serial.println("AllOn -> OFF (resuming patterns)");
    return;
  }

  // Trying to enable All-ON
  if (overheatShutdown) {
    server.send(200, "text/plain", "Cannot turn ALL ON: Overheat protection active.");
    return;
  }

  // Make manual override and allow relays
  relaysEnabled = true;
  manualOverride = true;

  prefs.begin("xmas", false);
  prefs.putBool("manualOverride", manualOverride);
  prefs.putBool("relaysEnabled", relaysEnabled);
  prefs.end();

  allOnMode = true;
  applyFrame(0xFF); // attempt to set all relays ON (setRelaySafe enforces dwell & overheat)
  server.send(200, "text/plain", "All relays ON (All-ON mode enabled). Click again to cancel.");
  Serial.println("AllOn -> ON (holding all relays on)");
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

  prefs.begin("xmas", false);
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
  prefs.begin("xmas", false);
  prefs.putBool("useSunset", useSunset);
  prefs.end();
  server.send(200, "text/plain", String("Sunset mode: ") + (useSunset ? "ENABLED" : "DISABLED"));
}

void handleSetSunsetOffset() {
  int m = server.arg("min").toInt();
  if (m < -180) m = -180;
  if (m >  180) m =  180;
  sunsetOffsetMin = m;
  prefs.begin("xmas", false);
  prefs.putInt("offset", sunsetOffsetMin);
  prefs.end();
  server.send(200, "text/plain", "Sunset offset set to " + String(sunsetOffsetMin) + " minutes");
}

void handleSetSpeed() {
  int val = server.arg("val").toInt();
  if (val < (int)minDwellMs) val = (int)minDwellMs;
  if (val > 2000) val = 2000;
  bool changed = (val != patternSpeed);
  patternSpeed = val;
  prefs.begin("xmas", false);
  if (changed) prefs.putInt("speed", patternSpeed);
  prefs.end();
  server.send(200, "text/plain", "Pattern speed set to " + String(patternSpeed) + " ms");
}

// When user manually picks a pattern, cancel All-ON so pattern takes effect immediately
void handlePattern() {
  if (allOnMode) {
    allOnMode = false;
    Serial.println("AllOn cancelled by manual pattern selection");
  }

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

  currentPattern = next;
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
  msg += "Shuffle: " + String(shuffleEnabled ? "ON" : "OFF") + "\n";
  msg += "All-ON: " + String(allOnMode ? "ON" : "OFF") + "\n";
  msg += "Hold: " + String(patternHoldMs / 1000UL) + " s\n";
  server.send(200, "text/plain", msg);
}

void handleStatusJSON() {
  DynamicJsonDocument doc(1024);
  struct tm tminfo;
  if (getLocalTime(&tminfo)) {
    doc["time"]["hour"] = tminfo.tm_hour;
    doc["time"]["min"]  = tminfo.tm_min;
  }
  doc["on"]["h"] = on_h; doc["on"]["m"] = on_m;
  doc["off"]["h"] = off_h; doc["off"]["m"] = off_m;
  doc["pattern"]  = patternName(currentPattern);
  doc["speed_ms"] = patternSpeed;
  doc["sunset"]["enabled"]     = useSunset;
  doc["sunset"]["offset_min"]  = sunsetOffsetMin;
  doc["relays_enabled"]        = relaysEnabled;
  doc["mode"]                  = manualOverride ? "MANUAL" : "AUTO";
  doc["playlist_len"] = (int)playlistLen;
  JsonArray order = doc.createNestedArray("play_order");
  for (size_t i=0;i<playlistLen;i++) order.add(patternName(playlist[i]));
  doc["shuffle"] = shuffleEnabled;
  doc["all_on"] = allOnMode;
  doc["hold_s"] = (int)(patternHoldMs / 1000UL);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ----------------------------- Playlist handlers --------------------------
void handleSetShuffle() {
  String arg = server.arg("enable");
  arg.trim();
  arg.toLowerCase();

  bool next;
  if (arg == "1" || arg == "true") next = true;
  else if (arg == "0" || arg == "false") next = false;
  else {
    server.send(200, "text/plain", String("Shuffle unchanged: ") + (shuffleEnabled ? "ON" : "OFF"));
    return;
  }

  bool changed = (next != shuffleEnabled);
  shuffleEnabled = next;

  prefs.begin("xmas", false);
  prefs.putBool("shuffle", shuffleEnabled);
  prefs.end();

  if (shuffleEnabled) {
    buildShuffledOrder(); playPos = 0; patternStart = millis();
    server.send(200, "text/plain", "Shuffle: ON (automatic rotation enabled)");
    Serial.println("Shuffle -> ON");
  } else {
    // Pause automatic rotation and keep current pattern running
    playPos = 0; patternStart = millis();
    server.send(200, "text/plain", "Shuffle: OFF (automatic rotation paused; staying on current pattern)");
    Serial.println("Shuffle -> OFF");
  }
}

void handleSetHold() {
  int sec = server.arg("sec").toInt();
  if (sec < 10) sec = 10;
  if (sec > 600) sec = 600;
  bool changed = (patternHoldMs != (uint32_t)sec * 1000UL);
  patternHoldMs = (uint32_t)sec * 1000UL;
  prefs.begin("xmas", false);
  if (changed) prefs.putInt("holdMs", (int)patternHoldMs);
  prefs.end();
  server.send(200, "text/plain", "Pattern hold set to " + String(sec) + " s");
}

// ----------------------------- Mount routes & WiFi -------------------------
void mountRoutes(bool apMode) {
  if (apMode) {
    server.onNotFound([]() {
      if (WiFi.getMode() & WIFI_AP) {
        server.sendHeader("Location", String("http://") + IPAddress(192,168,4,1).toString() + "/setup", true);
        server.send(302, "text/plain", "");
      } else server.send(404, "text/plain", "Not found");
    });
  }

  server.on("/", handleRootUI);
  server.on("/setup", handleSetupPage);
  server.on("/save", HTTP_POST, handleSaveSetup);

  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/auto", handleAuto);
  server.on("/allon", handleAllOn);

  server.on("/status", handleStatusText);
  server.on("/status.json", handleStatusJSON);

  server.on("/setschedule", handleSetSchedule);
  server.on("/setpattern", handlePattern);
  server.on("/pattern", handlePattern); // alias
  server.on("/setspeed", handleSetSpeed);
  server.on("/sunsetmode", handleSunsetMode);
  server.on("/setSunsetOffset", handleSetSunsetOffset);

  server.on("/setshuffle", handleSetShuffle);
  server.on("/sethold", handleSetHold);

  // OTA setup only if credentials are present in prefs
  prefs.begin("xmas", true);
  String otaUser = prefs.getString("ota_user", "");
  String otaPass = prefs.getString("ota_pass", "");
  prefs.end();
  if (otaUser.length() > 0 && otaPass.length() > 0) {
    httpUpdater.setup(&server, "/update", otaUser.c_str(), otaPass.c_str());
    Serial.println("OTA enabled");
  } else {
    Serial.println("OTA disabled (no creds in prefs)");
  }

  server.begin();
}

// ----------------------------- WiFi helpers -------------------------------
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
    Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi connect timeout.");
  return false;
}

void startAPWithPortal() {
  WiFi.mode(WIFI_AP);
  uint32_t chip = (uint32_t)ESP.getEfuseMac();
  char buf[32];
  snprintf(buf, sizeof(buf), "XMAS-SETUP-%04X", (unsigned)(chip & 0xFFFF));
  String apSSID = buf;

  IPAddress apIP(192,168,4,1);
  WiFi.softAP(apSSID.c_str());
  delay(100);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  dnsServer.start(53, "*", apIP);

  Serial.print("AP SSID: "); Serial.println(apSSID);
  Serial.print("Open http://"); Serial.println(apIP);

  mountRoutes(true);
}

void startMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS started: http://%s.local/\n", MDNS_NAME);
  } else Serial.println("mDNS start failed.");
}

// ----------------------------- Preferences loaders -------------------------
void loadPrefs() {
  prefs.begin("xmas", true);
  useSunset       = prefs.getBool("useSunset", true);
  sunsetOffsetMin = prefs.getInt("offset", 0);
  patternSpeed    = prefs.getInt("speed", 200);
  currentPattern  = (Pattern)prefs.getInt("pattern", (int)CHASE);
  on_h = prefs.getInt("on_h", 18);
  on_m = prefs.getInt("on_m", 0);
  off_h = prefs.getInt("off_h", 22);
  off_m = prefs.getInt("off_m", 0);
  manualOverride  = prefs.getBool("manualOverride", false);
  relaysEnabled   = prefs.getBool("relaysEnabled", false);
  shuffleEnabled  = prefs.getBool("shuffle", true);
  patternHoldMs   = (uint32_t)prefs.getInt("holdMs", 60000);
  prefs.end();
}

void loadCredentialsFromPrefs() {
  prefs.begin("xmas", true);
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  weatherApiKey = prefs.getString("weather_key", "");
  prefs.end();
}

// ----------------------------- Setup / Loop --------------------------------
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  initRelaysPins();
  dht.begin();

  loadPrefs();
  loadCredentialsFromPrefs();

  setenv("TZ", TZ_RULE, 1); tzset();
  configTime(0, 0, ntpServer);

  bool staConnected = connectWiFiStation();
  if (staConnected) {
    startMDNS();
    mountRoutes(false);
    struct tm tmp;
    if (getLocalTime(&tmp) && weatherApiKey.length()) {
      if (updateSunsetTime()) lastSunsetUpdate = millis();
      else lastSunsetUpdate = 0;
    } else lastSunsetUpdate = 0;
  } else startAPWithPortal();

  if (shuffleEnabled) buildShuffledOrder(); else buildSequentialOrder();
  patternStart = millis();
  if (relaysEnabled) patternStart = millis();
}

void loop() {
  server.handleClient();
  if (WiFi.getMode() & WIFI_AP) dnsServer.processNextRequest();

  if (rebootPending && (millis() - rebootArmedAt >= REBOOT_DELAY_MS)) {
    rebootPending = false;
    ESP.restart();
  }

  // Sunset update daily (~24h)
  if ((WiFi.status() == WL_CONNECTED) && useSunset && weatherApiKey.length()) {
    if ((unsigned long)(millis() - lastSunsetUpdate) > 86400000UL) {
      if (updateSunsetTime()) lastSunsetUpdate = millis();
      else lastSunsetUpdate = millis() - 86400000UL + 600000UL;
    }
  }

  checkTemperatureAndProtect();

  // Schedule check every second (AUTO)
  if (!manualOverride && (unsigned long)(millis() - lastScheduleCheck) > 1000UL) {
    lastScheduleCheck = millis();
    bool shouldBeOn = computeShouldBeOn();
    if (shouldBeOn != relaysEnabled) {
      relaysEnabled = shouldBeOn;
      prefs.begin("xmas", false);
      prefs.putBool("relaysEnabled", relaysEnabled);
      prefs.end();
      alreadyOff = !relaysEnabled;
      if (relaysEnabled) patternStart = millis();
      else {
        // When schedule forces OFF, cancel All-ON
        allOnMode = false;
        forceAllRelaysOff();
      }
    }
  }

  // If All-ON mode is active and relays allowed, keep trying to apply all-on frame
  if (allOnMode && relaysEnabled && !overheatShutdown) {
    applyFrame(0xFF); // apply all-on repeatedly (setRelaySafe will enforce dwell)
    // do not step patterns or advance playlist while in All-ON mode
    // but still allow schedule to turn relays off (above)
    return;
  }

  // Playlist rotation (may be paused by shuffleOff or allOnMode)
  maybeAdvancePattern();

  // Drive patterns if relays are enabled and not in allOnMode
  if (relaysEnabled && !overheatShutdown) {
    alreadyOff = false;
    stepPatternIfDue();
  } else {
    if (!alreadyOff) { forceAllRelaysOff(); alreadyOff = true; }
  }
}