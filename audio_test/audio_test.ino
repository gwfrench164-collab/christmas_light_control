#include <WiFi.h>
#include <Adafruit VS1053 Library.h>

#define VS_CS    5
#define VS_DCS   22
#define VS_DREQ  4
#define VS_RST   21  // XRST

#define SPI_CLK   18
#define SPI_MOSI  23
#define SPI_MISO  19

ESP32_VS1053_Stream player;

const char* ssid     = "";
const char* password = "";
const char* streamURL = "http://maestro.emfcdn.com/stream_for/k-love/iheart/aac";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI);
  if (!player.startDecoder(VS_CS, VS_DCS, VS_DREQ, VS_RST)) {
    Serial.println("VS1053 init failed!");
    while (1);
  }
  Serial.println("VS1053 initialized");

  player.connecttohost(streamURL);
  Serial.println("Streaming started");
}

void loop() {
  player.loop();  // keep feeding the stream
}
