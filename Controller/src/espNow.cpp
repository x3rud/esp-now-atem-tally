#include <Arduino.h>
#include <climits>
#include <cstring>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "atem.h"
#include "espnow.h"
#include "vmixServer.h"
#include "configWebserver.h" // for broadcastState declaration

// Broadcast address, sends to all devices nearby
uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
espnow_tally_info_t tallies[MAX_TALLY_COUNT];
uint64_t programBits = 0;
uint64_t previewBits = 0;
long lastMessageAt = -10000;

espnow_tally_info_t * espnow_tallies() {
  return tallies;
}

uint8_t espnow_total_tally_count() {
  uint8_t total = 0;
  for (int i = 0; i < MAX_TALLY_COUNT; i++) {
    if (tallies[i].id != 0) total++;
  }
  return total;
}

uint8_t espnow_active_tally_count(unsigned long freshnessMs) {
  unsigned long now = millis();
  uint8_t active = 0;
  for (int i = 0; i < MAX_TALLY_COUNT; i++) {
    if (tallies[i].id == 0) continue;
    if (now - tallies[i].last_seen <= freshnessMs) active++;
  }
  return active;
}

unsigned long espnow_latest_heartbeat_age() {
  unsigned long now = millis();
  bool hasEntry = false;
  unsigned long youngest = 0;
  for (int i = 0; i < MAX_TALLY_COUNT; i++) {
    if (tallies[i].id == 0) continue;
    unsigned long age = now - tallies[i].last_seen;
    if (!hasEntry || age < youngest) {
      youngest = age;
      hasEntry = true;
    }
  }
  return hasEntry ? youngest : ULONG_MAX;
}

void espnow_set_name(const String& name, uint64_t *bits) {
  if (name.length() == 0 || bits == nullptr) return;
  const uint8_t maxName = 16;
  uint8_t nameLen = name.length() > maxName ? maxName : name.length();
  uint8_t payload[2 + maxName + sizeof(uint64_t)];
  payload[0] = SET_NAME;
  payload[1] = nameLen;
  memcpy(payload + 2, name.c_str(), nameLen);
  memcpy(payload + 2 + nameLen, bits, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcast_mac, payload, 2 + nameLen + sizeof(uint64_t));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (SET_NAME)");
}

void espnow_tally() {
  espnow_tally(&programBits, &previewBits);
}

void espnow_tally(uint64_t *program, uint64_t *preview) {
  uint8_t payload[1+sizeof(uint64_t)+sizeof(uint64_t)];
  payload[0] = SET_TALLY;
  memcpy(payload+1, program, sizeof(uint64_t));
  memcpy(payload+1+sizeof(uint64_t), preview, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  programBits = *program;
  previewBits = *preview;
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
  vmix_tally(program, preview);
  broadcastState();
}

void switchCamId(uint8_t id1, uint8_t id2) {
  uint8_t payload[3] = {SWITCH_CAMID, id1, id2};
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_brightness(uint8_t brightness, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_BRIGHTNESS;
  payload[1] = brightness;
  memcpy(payload+2, bits, sizeof(*bits));
  if (!esp_now_send(broadcast_mac, payload, sizeof(payload))) {
    Serial.println("esp_now_send != OK");
  }
}

void espnow_brightness_mac(uint8_t brightness, const uint8_t mac[6]) {
  if (!mac) return;
  uint8_t payload[1 + 1 + 6];
  payload[0] = SET_BRIGHTNESS_MAC;
  payload[1] = brightness;
  memcpy(payload + 2, mac, 6);
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (BRIGHTNESS_MAC)");
}

void espnow_camid(uint8_t camId, uint64_t *bits) {
  uint8_t payload[2+sizeof(uint64_t)];
  payload[0] = SET_CAMID;
  payload[1] = camId;
  memcpy(payload+2, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_color(uint32_t color, uint64_t *bits) {
  // "/rgba\0\0\0,ir\0{tallyid}{color}"
  uint8_t payload[4+sizeof(uint64_t)];
  payload[0] = SET_COLOR;
  payload[1] = (color >> 16) & 0xFF;
  payload[2] = (color >> 8) & 0xFF;
  payload[3] = color & 0xFF;
  memcpy(payload+4, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_signal(uint8_t signal, uint64_t *bits) {
  // "/signal\0,ii\0{tallyid}{signal}"
  uint8_t payload[1+sizeof(uint64_t)];
  payload[0] = signal;  // Signal number is command number
  memcpy(payload+1, bits, sizeof(*bits));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK");
}

void espnow_identify(uint64_t *bits, uint8_t seconds) {
  uint8_t payload[2 + sizeof(uint64_t)];
  payload[0] = SET_IDENTIFY;
  payload[1] = seconds;
  memcpy(payload + 2, bits, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (IDENTIFY)");
}

void espnow_identify_mac(const uint8_t mac[6], uint8_t seconds) {
  if (!mac) return;
  uint8_t payload[1 + 1 + 6];
  payload[0] = SET_IDENTIFY;
  payload[1] = seconds;
  memcpy(payload + 2, mac, 6);
  // broadcast the payload; receivers compare mac internally
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (IDENTIFY MAC)");
}

void espnow_blink(uint32_t color, bool enable, uint64_t *bits) {
  uint8_t payload[2 + 3 + sizeof(uint64_t)];
  payload[0] = SET_BLINK;
  payload[1] = enable ? 1 : 0;
  payload[2] = (color >> 16) & 0xFF;
  payload[3] = (color >> 8) & 0xFF;
  payload[4] = color & 0xFF;
  memcpy(payload + 5, bits, sizeof(uint64_t));
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (BLINK)");
}

void espnow_set_camid_mac(uint8_t camId, const uint8_t mac[6]) {
  if (!mac || camId == 0 || camId > MAX_TALLY_COUNT) return;
  uint8_t payload[1 + 1 + 6];
  payload[0] = SET_CAMID_MAC;
  payload[1] = camId;
  memcpy(payload + 2, mac, 6);
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (CAMID_MAC)");
}

void espnow_set_name_mac(const String& name, const uint8_t mac[6]) {
  if (!mac || name.length() == 0) return;
  const uint8_t maxName = 16;
  uint8_t nameLen = name.length() > maxName ? maxName : name.length();
  uint8_t payload[1 + 1 + maxName + 6];
  payload[0] = SET_NAME_MAC;
  payload[1] = nameLen;
  memcpy(payload + 2, name.c_str(), nameLen);
  memcpy(payload + 2 + nameLen, mac, 6);
  esp_err_t result = esp_now_send(broadcast_mac, payload, 2 + nameLen + 6);
  if (result != ESP_OK) Serial.println("esp_now_send != OK (NAME_MAC)");
}

void espnow_status_brightness(uint8_t brightness, const uint8_t mac[6]) {
  if (!mac) return;
  uint8_t payload[1 + 1 + 6];
  payload[0] = SET_STATUS_BRIGHTNESS;
  payload[1] = brightness;
  memcpy(payload + 2, mac, 6);
  esp_err_t result = esp_now_send(broadcast_mac, payload, sizeof(payload));
  if (result != ESP_OK) Serial.println("esp_now_send != OK (STATUS_BRIGHTNESS)");
}

// callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  // Serial.print("\r\nLast Packet Send Status:\t");
  // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

// callback when data is received
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
  espnow_command command = (espnow_command) data[0];
  // Serial.printf("Command[%d]: ", len);
  switch (command)
  {
  case HEARTBEAT: {
    // data[1] = id, data[2] = rgb, data[3] = status, data[8] = signal, data[9] = nameLen, data[10..] = name
    int8_t signal = (int8_t)data[8];
    uint8_t rgb = len > 2 ? data[2] : 255;
    uint8_t status = len > 3 ? data[3] : 255;
    uint8_t nameLen = 0;
    const char* namePtr = nullptr;
    if (len > 10) {
      nameLen = data[9];
      if (nameLen > len - 10) nameLen = len - 10;
      namePtr = (const char*)(data + 10);
    }
    int freeIdx = -1;
    for (int i=0; i<MAX_TALLY_COUNT; i++) {
      if (tallies[i].id == 0 && freeIdx < 0) freeIdx = i;
      if (memcmp(tallies[i].mac_addr, mac_addr, 6) == 0) {
        tallies[i].id = data[1];
        tallies[i].last_seen = millis();
        tallies[i].signal = signal;
        tallies[i].rgbBrightness = rgb;
        tallies[i].statusBrightness = status;
        if (nameLen > 0) {
          uint8_t l = nameLen > 16 ? 16 : nameLen;
          memcpy(tallies[i].name, namePtr, l);
          tallies[i].name[l] = 0;
        }
        freeIdx = -1; // handled
        break;
      }
    }
    if (freeIdx >= 0) {
      memcpy(tallies[freeIdx].mac_addr, mac_addr, 6);
      tallies[freeIdx].id = data[1];
      tallies[freeIdx].last_seen = millis();
      tallies[freeIdx].signal = signal;
      tallies[freeIdx].rgbBrightness = rgb;
      tallies[freeIdx].statusBrightness = status;
      if (nameLen > 0) {
        uint8_t l = nameLen > 16 ? 16 : nameLen;
        memcpy(tallies[freeIdx].name, namePtr, l);
        tallies[freeIdx].name[l] = 0;
      } else {
        tallies[freeIdx].name[0] = 0;
      }
    }
    broadcastState();
    break;
  }
  
  case GET_TALLY:
    Serial.println("GET_TALLY");
    espnow_tally();
    break;
  
  default:
    break;
  }
}

void espnow_setup()
{
  Serial.println("SetupEspNow");
  WiFi.mode(WIFI_STA);
  // config long range mode
  int a = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  Serial.println(a);
  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init != OK");
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Transmitted packet and register peer data receive
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register peer
  memcpy(peerInfo.peer_addr, broadcast_mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  
  // Zero tallies array
  for (int i=0; i<MAX_TALLY_COUNT; i++) {
    tallies[i].id = 0;
    tallies[i].name[0] = 0;
    tallies[i].signal = 0;
    tallies[i].rgbBrightness = 255;
    tallies[i].statusBrightness = 255;
  }

  vmixServerSetup();
}

void espnow_loop() {
  if (millis() - lastMessageAt > TALLY_UPDATE_EACH) {
    espnow_tally();
    lastMessageAt = millis();
  }

  vmixServerLoop();
}
