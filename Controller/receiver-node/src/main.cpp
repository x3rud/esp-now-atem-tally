#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WebServer.h>

// Pin map for Wemos D1 mini (ESP8266)
// D1 -> GPIO5 (LED R), D2 -> GPIO4 (LED G), D3 -> GPIO0 (LED B)
// D6 -> GPIO12 (status LED), D7 -> GPIO13 (button)
// D5 -> GPIO14 (identify LED - green)
constexpr uint8_t PIN_LED_R = D1;
constexpr uint8_t PIN_LED_G = D2;
constexpr uint8_t PIN_LED_B = D3;
constexpr uint8_t PIN_STATUS = D6;
constexpr uint8_t PIN_BUTTON = D7;
constexpr uint8_t PIN_IDENT = D5;
constexpr uint8_t PIN_WS = D1;  // single-wire pin for WS2812

#ifndef OTA_SSID
#define OTA_SSID ""
#endif
#ifndef OTA_PASS
#define OTA_PASS ""
#endif

// ESP-NOW commands (must match controller)
enum espnow_command : uint8_t {
  SET_TALLY = 1,
  GET_TALLY = 2,
  SWITCH_CAMID = 3,
  HEARTBEAT = 4,
  SET_CAMID = 5,
  SET_COLOR = 6,
  SET_BRIGHTNESS = 7,
  SET_SIGNAL = 8,
  SET_NAME = 9,
  SET_IDENTIFY = 10,
  SET_BLINK = 11,
  SET_CAMID_MAC = 12,
  SET_NAME_MAC = 13,
  SET_BRIGHTNESS_MAC = 14,
  SET_STATUS_BRIGHTNESS = 15
};

constexpr uint8_t MAX_TALLIES = 64;
constexpr unsigned long HEARTBEAT_INTERVAL = 2000;
constexpr unsigned long LINK_TIMEOUT = 5000;

uint8_t tallyId = 1;
uint64_t programBits = 0;
uint64_t previewBits = 0;
uint8_t rgbBrightness = 255;
uint8_t statusBrightness = 255;
unsigned long lastPacketAt = 0;
unsigned long lastHeartbeatAt = 0;
bool colorOverride = false;
uint32_t overrideColor = 0;
char camName[32] = "CAM";
bool identifyActive = false;
unsigned long identifyUntil = 0;
uint8_t selfMac[6] = {0};
bool blinkActive = false;
uint32_t blinkColor = 0;
unsigned long blinkNextToggle = 0;
bool blinkState = false;
bool otaEnabled = false;
enum led_type : uint8_t { LED_RGB = 0, LED_WS2812 = 1 };
led_type ledType =
#ifdef LED_TYPE_WS2812
  LED_WS2812;
#else
  LED_RGB;
#endif
Adafruit_NeoPixel strip(1, PIN_WS, NEO_GRB + NEO_KHZ800);
ESP8266WebServer apiServer(80);

uint64_t bitn(uint8_t n) { return (uint64_t)1 << (n - 1); }

bool isProgram() { return programBits & bitn(tallyId); }
bool isPreview() { return previewBits & bitn(tallyId); }

// Map 0-255 brightness to PWM and write to RGB LED (common cathode assumed).
void writeRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (ledType == LED_WS2812) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
  } else {
    analogWrite(PIN_LED_R, r);
    analogWrite(PIN_LED_G, g);
    analogWrite(PIN_LED_B, b);
  }
}

void setTallyLeds() {
  uint8_t r = 0, g = 0, b = 0;
  if (blinkActive) {
    // blink uses blinkColor when state is on; off means LEDs off
    if (blinkState) {
      r = (blinkColor >> 16) & 0xFF;
      g = (blinkColor >> 8) & 0xFF;
      b = blinkColor & 0xFF;
    } else {
      r = g = b = 0;
    }
  } else if (colorOverride) {
    r = (overrideColor >> 16) & 0xFF;
    g = (overrideColor >> 8) & 0xFF;
    b = overrideColor & 0xFF;
  } else if (isProgram() && isPreview()) {
    r = 255; g = 128; b = 0;  // orange if both
  } else if (isProgram()) {
    r = 255;
  } else if (isPreview()) {
    g = 255;
  }

  // Apply global brightness
  r = (r * rgbBrightness) / 255;
  g = (g * rgbBrightness) / 255;
  b = (b * rgbBrightness) / 255;
  writeRgb(r, g, b);
}

void handleSetColor(const uint8_t* data, int len) {
  if (len < 4 + (int)sizeof(uint64_t)) return;
  uint32_t color = (data[1] << 16) | (data[2] << 8) | data[3];
  uint64_t bits = 0;
  memcpy(&bits, data + 4, sizeof(bits));
  if (bits & bitn(tallyId)) {
    overrideColor = color;
    colorOverride = true;
    setTallyLeds();
  }
}

void handleSetBrightness(const uint8_t* data, int len) {
  if (len < 2 + (int)sizeof(uint64_t)) return;
  uint64_t bits = 0;
  memcpy(&bits, data + 2, sizeof(bits));
  if (bits & bitn(tallyId)) {
    rgbBrightness = data[1];
    EEPROM.write(40, rgbBrightness);
    EEPROM.commit();
    setTallyLeds();
  }
}

void handleSetBrightnessMac(const uint8_t* data, int len) {
  if (len < 1 + 1 + 6) return;
  if (memcmp(selfMac, data + 2, 6) != 0) return;
  rgbBrightness = data[1];
  EEPROM.write(40, rgbBrightness);
  EEPROM.commit();
  setTallyLeds();
}

void handleSetCamId(const uint8_t* data, int len) {
  if (len < 2 + (int)sizeof(uint64_t)) return;
  uint8_t newId = data[1];
  if (newId == 0 || newId > MAX_TALLIES) return;
  uint64_t bits = 0;
  memcpy(&bits, data + 2, sizeof(bits));
  if (bits & bitn(tallyId)) {
    tallyId = newId;
  }
}

void handleSetCamIdMac(const uint8_t* data, int len) {
  if (len < 1 + 1 + 6) return;
  uint8_t newId = data[1];
  if (newId == 0 || newId > MAX_TALLIES) return;
  if (memcmp(selfMac, data + 2, 6) != 0) return;
  tallyId = newId;
}

void saveNameToEeprom(uint8_t nameLen) {
  EEPROM.write(0, nameLen);
  for (uint8_t i = 0; i < nameLen; i++) EEPROM.write(1 + i, camName[i]);
  EEPROM.commit();
}

void handleSetName(const uint8_t* data, int len) {
  if (len < 3 + (int)sizeof(uint64_t)) return;
  uint8_t nameLen = data[1];
  if (nameLen > 31) nameLen = 31;
  if (nameLen > len - 2 - (int)sizeof(uint64_t)) return;
  uint64_t bits = 0;
  memcpy(&bits, data + 2 + nameLen, sizeof(bits));
  if (!(bits & bitn(tallyId))) return;
  memcpy(camName, data + 2, nameLen);
  camName[nameLen] = 0;
  saveNameToEeprom(nameLen);
  Serial.printf("Name set: %s\n", camName);
}

void handleSetNameMac(const uint8_t* data, int len) {
  if (len < 1 + 1 + 6) return;
  uint8_t nameLen = data[1];
  if (nameLen > 31) nameLen = 31;
  if (nameLen > len - 2 - 6) return;
  if (memcmp(selfMac, data + 2 + nameLen, 6) != 0) return;
  memcpy(camName, data + 2, nameLen);
  camName[nameLen] = 0;
  saveNameToEeprom(nameLen);
  Serial.printf("Name set: %s\n", camName);
}

void handleIdentify(const uint8_t* data, int len) {
  if (len < 2) return;
  uint8_t seconds = data[1];
  if (len >= 1 + 1 + 6 && len < 1 + 1 + (int)sizeof(uint64_t) + 1) {
    // MAC-targeted payload
    if (memcmp(selfMac, data + 2, 6) != 0) return;
  } else if (len >= 1 + 1 + (int)sizeof(uint64_t)) {
    // Legacy bitmask payload
    uint64_t bits = 0;
    memcpy(&bits, data + 2, sizeof(bits));
    if (!(bits & bitn(tallyId))) return;
  } else {
    return;
  }
  identifyActive = true;
  identifyUntil = millis() + (unsigned long)seconds * 1000;
}

void handleBlink(const uint8_t* data, int len) {
  if (len < 5 + (int)sizeof(uint64_t)) return;
  bool enable = data[1] != 0;
  uint32_t color = (data[2] << 16) | (data[3] << 8) | data[4];
  uint64_t bits = 0;
  memcpy(&bits, data + 5, sizeof(bits));
  if (!(bits & bitn(tallyId))) return;
  blinkActive = enable;
  blinkColor = color;
  blinkState = true;
  blinkNextToggle = millis() + 400;
  setTallyLeds();
}

void handleStatusBrightness(const uint8_t* data, int len) {
  if (len < 1 + 1 + 6) return;
  uint8_t b = data[1];
  if (memcmp(selfMac, data + 2, 6) != 0) return;
  statusBrightness = b;
  EEPROM.write(41, statusBrightness);
  EEPROM.commit();
}

void handleLedType(const uint8_t* data, int len) {
  if (len < 1 + 1 + 6) return;
  if (memcmp(selfMac, data + 2, 6) != 0) return;
  ledType = (data[1] == LED_WS2812) ? LED_WS2812 : LED_RGB;
}

void handleSetTally(const uint8_t* data, int len) {
  if (len < 1 + 2 * (int)sizeof(uint64_t)) return;
  memcpy(&programBits, data + 1, sizeof(programBits));
  memcpy(&previewBits, data + 1 + sizeof(uint64_t), sizeof(previewBits));
  colorOverride = false;  // reset overrides on fresh tally update
  setTallyLeds();
}

void handleSwitchCam(const uint8_t* data, int len) {
  if (len < 3) return;
  uint8_t from = data[1];
  uint8_t to = data[2];
  if (tallyId == from) {
    tallyId = to;
  }
}

void OnDataRecv(uint8_t *mac_addr, uint8_t *data, uint8_t len) {
  if (len == 0) return;
  lastPacketAt = millis();
  switch ((espnow_command)data[0]) {
    case SET_TALLY:
      handleSetTally(data, len);
      break;
    case SET_COLOR:
      handleSetColor(data, len);
      break;
    case SET_BRIGHTNESS:
      handleSetBrightness(data, len);
      break;
    case SET_BRIGHTNESS_MAC:
      handleSetBrightnessMac(data, len);
      break;
    case SET_CAMID:
      handleSetCamId(data, len);
      break;
    case SET_CAMID_MAC:
      handleSetCamIdMac(data, len);
      break;
    case SET_NAME:
      handleSetName(data, len);
      break;
    case SET_NAME_MAC:
      handleSetNameMac(data, len);
      break;
    case SET_IDENTIFY:
      handleIdentify(data, len);
      break;
    case SET_BLINK:
      handleBlink(data, len);
      break;
    case SET_STATUS_BRIGHTNESS:
      handleStatusBrightness(data, len);
      break;
    case SWITCH_CAMID:
      handleSwitchCam(data, len);
      break;
    default:
      break;
  }
}

void sendHeartbeat() {
  uint8_t nameLen = strlen(camName);
  if (nameLen > 16) nameLen = 16;  // limit size on the wire
  uint8_t payload[11 + 16];
  memset(payload, 0, sizeof(payload));
  payload[0] = HEARTBEAT;
  payload[1] = tallyId;
  payload[2] = rgbBrightness;
  payload[3] = statusBrightness;
  int8_t rssi = WiFi.RSSI();
  payload[8] = (uint8_t)rssi; // send signed RSSI as raw byte
  payload[9] = nameLen;
  memcpy(payload + 10, camName, nameLen);
  uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_send(broadcastAddr, payload, 10 + nameLen);
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);

  // Add broadcast peer
  uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_add_peer(broadcastAddr, ESP_NOW_ROLE_COMBO, 0, NULL, 0);
}

void handleButton() {
  static bool lastState = HIGH;
  static unsigned long lastChange = 0;
  bool state = digitalRead(PIN_BUTTON);
  unsigned long now = millis();
  if (state != lastState && now - lastChange > 50) {
    lastChange = now;
    lastState = state;
    if (state == LOW) {  // pressed
      tallyId = (tallyId % MAX_TALLIES) + 1;
      Serial.printf("Tally ID -> %u\n", tallyId);
      colorOverride = false;
      setTallyLeds();
    }
  }
}

void setupPins() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_STATUS, OUTPUT);
  pinMode(PIN_IDENT, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  analogWriteRange(255);
  writeRgb(0, 0, 0);
  analogWrite(PIN_STATUS, 0);
  digitalWrite(PIN_IDENT, LOW);
  strip.begin();
  strip.show();
}

void loadConfig() {
  EEPROM.begin(64);
  uint8_t len = EEPROM.read(0);
  if (len == 0xFF || len == 0 || len > 31) {
    strncpy(camName, "CAM", sizeof(camName));
    camName[sizeof(camName)-1] = 0;
  } else {
    for (uint8_t i = 0; i < len; i++) {
      camName[i] = EEPROM.read(1 + i);
    }
    camName[len] = 0;
  }
  // brightness slots
  rgbBrightness = EEPROM.read(40);
  if (rgbBrightness == 0xFF) rgbBrightness = 255;
  statusBrightness = EEPROM.read(41);
  if (statusBrightness == 0xFF) statusBrightness = 255;
}

void handleApiSet() {
  if (apiServer.hasArg("program")) {
    uint8_t pgm = apiServer.arg("program").toInt();
    programBits = (pgm > 0 && pgm <= MAX_TALLIES) ? bitn(pgm) : 0;
  }
  if (apiServer.hasArg("preview")) {
    uint8_t pvw = apiServer.arg("preview").toInt();
    previewBits = (pvw > 0 && pvw <= MAX_TALLIES) ? bitn(pvw) : 0;
  }
  if (apiServer.hasArg("blink")) {
    uint32_t color = strtoul(apiServer.arg("blink").c_str(), nullptr, 16);
    blinkActive = true;
    blinkColor = color;
    blinkState = true;
    blinkNextToggle = millis() + 400;
  }
  if (apiServer.hasArg("off")) {
    blinkActive = false;
  }
  setTallyLeds();
  apiServer.send(200, "text/plain", "OK");
}

void setupApiServer() {
  apiServer.on("/set", HTTP_GET, handleApiSet);
  apiServer.begin();
}

void setupOta() {
  if (strlen(OTA_SSID) == 0) return;
  WiFi.begin(OTA_SSID, OTA_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA WiFi connect failed");
    return;
  }
  ArduinoOTA.setHostname("tally-rx");
  ArduinoOTA.begin();
  otaEnabled = true;
  Serial.printf("OTA ready at %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  setupPins();
  loadConfig();
  WiFi.macAddress(selfMac);
  setupEspNow();
  setupApiServer();
  setupOta();
  lastPacketAt = millis();
  lastHeartbeatAt = millis();
}

void loop() {
  unsigned long now = millis();

  // Status LED: on when recently received data
  bool linkAlive = (now - lastPacketAt) < LINK_TIMEOUT;
  uint8_t statusLevel = linkAlive ? statusBrightness : 0;
  analogWrite(PIN_STATUS, statusLevel);

  // Identify LED
  if (identifyActive) {
    if (now >= identifyUntil) {
      identifyActive = false;
      digitalWrite(PIN_IDENT, LOW);
    } else {
      digitalWrite(PIN_IDENT, (now / 200) % 2 ? HIGH : LOW);
    }
  } else {
    digitalWrite(PIN_IDENT, LOW);
  }

  // Blink handling
  if (blinkActive && now >= blinkNextToggle) {
    blinkState = !blinkState;
    blinkNextToggle = now + 400;
    setTallyLeds();
  }

  if (now - lastHeartbeatAt > HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatAt = now;
  }

  apiServer.handleClient();
  if (otaEnabled) {
    ArduinoOTA.handle();
  }

  handleButton();
}
