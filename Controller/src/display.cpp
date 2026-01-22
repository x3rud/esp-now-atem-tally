#include "display.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ETH.h>
#include <WiFi.h>
#include <Wire.h>
#include <climits>
#include "espnow.h"
#include "main.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#ifndef OLED_I2C_ADDRESS
#define OLED_I2C_ADDRESS 0x3C
#endif
#ifndef OLED_SDA_PIN
  // WT32-ETH01 has I2C-friendly GPIO33/32 available; fall back to 21/22 elsewhere.
  #if defined(ARDUINO_WT32_ETH01) || defined(WT32_ETH01)
    #define OLED_SDA_PIN 33
  #else
    #define OLED_SDA_PIN 21
  #endif
#endif
#ifndef OLED_SCL_PIN
  #if defined(ARDUINO_WT32_ETH01) || defined(WT32_ETH01)
    #define OLED_SCL_PIN 32
  #else
    #define OLED_SCL_PIN 22
  #endif
#endif
#define DISPLAY_REFRESH_MS 1000
#define TALLY_STALE_MS 5000

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool displayReady = false;
static unsigned long lastRefresh = 0;

static const char* protocolLabel(switcher_protocol protocol) {
  switch (protocol) {
    case PROTOCOL_ATEM:
      return "ATEM";
    case PROTOCOL_OBS:
      return "OBS";
    case PROTOCOL_VMIX:
      return "VMIX";
    default:
      return "UNKNOWN";
  }
}

static IPAddress currentIp() {
  IPAddress ip;
  if (ETH.linkUp()) {
    ip = ETH.localIP();
    if (ip) return ip;
  }

  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP();
    if (ip) return ip;
  }

  return IPAddress(0U, 0U, 0U, 0U);
}

static void drawStatus() {
  IPAddress ip = currentIp();
  uint8_t totalTallies = espnow_total_tally_count();
  uint8_t activeTallies = espnow_active_tally_count(TALLY_STALE_MS);
  unsigned long lastHbAge = espnow_latest_heartbeat_age();
  int bestSignal = -128;
  espnow_tally_info_t *tallies = espnow_tallies();
  unsigned long now = millis();
  for (int i=0; i<MAX_TALLY_COUNT; i++) {
    if (tallies[i].id == 0) continue;
    if (now - tallies[i].last_seen > TALLY_STALE_MS) continue;
    if (tallies[i].signal > bestSignal) bestSignal = tallies[i].signal;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Tally Bridge");

  display.print("Mode: ");
  display.println(protocolLabel(config.protocol));

  display.print("IP: ");
  if (ip == IPAddress(0U, 0U, 0U, 0U)) display.println("None");
  else display.println(ip);

  display.print("Tallies: ");
  display.print(activeTallies);
  display.print("/");
  display.println(totalTallies);

  display.print("Last HB: ");
  if (lastHbAge == ULONG_MAX) display.println("--");
  else {
    display.print(lastHbAge / 1000);
    display.println("s ago");
  }

  display.print("RSSI: ");
  if (bestSignal == -128) display.println("--");
  else {
    display.print(bestSignal);
    display.print("dBm ");
    if (bestSignal > -60) display.println("G");       // good
    else if (bestSignal > -75) display.println("M");  // mid
    else display.println("L");                        // low
  }

  display.display();
}

void statusDisplaySetup() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
  if (!displayReady) {
    Serial.println("SSD1306 init failed");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("OLED online");
  display.display();
}

void statusDisplayLoop() {
  if (!displayReady) return;

  unsigned long now = millis();
  if (now - lastRefresh < DISPLAY_REFRESH_MS) return;
  lastRefresh = now;

  drawStatus();
}
