#pragma once

#include <Arduino.h>
#include <esp_now.h>

extern uint64_t programBits;
extern uint64_t previewBits;
extern long lastMessageTime;

#define MAX_TALLY_COUNT 64

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
  SET_STATUS_BRIGHTNESS = 15,
};

typedef struct esp_now_tally_info {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t id;
    unsigned long last_seen;
    char name[17];
    int8_t signal;
    uint8_t rgbBrightness;
    uint8_t statusBrightness;
} espnow_tally_info_t;

espnow_tally_info_t * espnow_tallies();
uint8_t espnow_active_tally_count(unsigned long freshnessMs = 5000);
uint8_t espnow_total_tally_count();
unsigned long espnow_latest_heartbeat_age();
void espnow_set_name(const String& name, uint64_t *bits);

void espnow_setup();
void espnow_loop();
void espnow_brightness(uint8_t brightness, uint64_t *bits);
void espnow_camid(uint8_t camId, uint64_t *bits);
void espnow_color(uint32_t, uint64_t *bits);
void espnow_signal(uint8_t signal, uint64_t *bits);
void espnow_tally();
void espnow_tally(uint64_t *program, uint64_t *preview);
void espnow_tally_test(int pgm, int pvw);
void espnow_identify(uint64_t *bits, uint8_t seconds);
void espnow_identify_mac(const uint8_t mac[6], uint8_t seconds);
void espnow_blink(uint32_t color, bool enable, uint64_t *bits);
void espnow_set_camid_mac(uint8_t camId, const uint8_t mac[6]);
void espnow_set_name_mac(const String& name, const uint8_t mac[6]);
void espnow_brightness_mac(uint8_t brightness, const uint8_t mac[6]);
void espnow_status_brightness(uint8_t brightness, const uint8_t mac[6]);
