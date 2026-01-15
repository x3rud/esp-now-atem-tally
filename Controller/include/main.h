#pragma once

#include <Arduino.h>

enum switcher_protocol : uint8_t {
    PROTOCOL_ATEM = 1,
    PROTOCOL_OBS  = 2,
    PROTOCOL_VMIX = 3,
};

struct controller_config {
    switcher_protocol protocol = PROTOCOL_ATEM;
    uint8_t reserved = 0xFF;  // legacy placeholder to keep EEPROM layout stable
    uint32_t atemIP = (192<<24)+(168<<16)+(2<<8)+240;
    uint16_t atemPort = 9910;
    uint32_t obsIP = (192<<24)+(168<<16)+(2<<8)+18;
    uint16_t obsPort = 4455;
    uint32_t vmixIP = (192<<24)+(168<<16)+(2<<8)+18;
    uint16_t vmixPort = 8099;
    bool protocolEnabled = true;
};

extern struct controller_config config;

void readConfig();
void writeConfig();
