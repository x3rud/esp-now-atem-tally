#include <Arduino.h>
#include <ATEMbase.h>
#include <ATEMstd.h>
#include <EEPROM.h>
#include <mdns.h>
#include <ArduinoOTA.h>

#include "espnow.h"
#include "configWebserver.h"
#include "atem.h"
#include "obs.h"
#include "vmix.h"
#include "display.h"

struct controller_config config;
static bool protocolRunning = false;

void startProtocol();
void stopProtocol();

void readConfig()
{
  Serial.println("readConfig");
  EEPROM.begin(512);
  EEPROM.get(0, config);
  if (config.atemIP == 0xFFFFFFFF) {
    // Set defaults
    config.protocol = PROTOCOL_ATEM;
    // config.atemIP = IPAddress(192,168,2,240);
    config.atemIP = (uint32_t)IPAddress(192,168,88,240);
    config.atemPort = 9910;
    // config.obsIP = IPAddress(192,168,2,24);
    config.obsIP = (uint32_t)IPAddress(192,168,88,21);
    config.obsPort = 4455;
    config.protocolEnabled = true;
  } else {
    if (config.protocolEnabled != 0 && config.protocolEnabled != 1) {
      config.protocolEnabled = true;
    }
  }
  EEPROM.end();
}	

void writeConfig()
{
  Serial.println("writeConfig");
  EEPROM.begin(512);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
    delay(5);

  // Set LED pin to output mode
  // pinMode(ERROR_LED_PIN, OUTPUT);
  // digitalWrite(ERROR_LED_PIN, HIGH);
  readConfig();
  Serial.printf("protocol=%d\n", config.protocol);
  setupWebserver();
  espnow_setup();
  statusDisplaySetup();
  
  if (esp_err_t err = mdns_init()) {
    Serial.printf("MDNS Init failed: %d\n", err);
  }
  mdns_hostname_set("tally");
  mdns_instance_name_set("TallyBridge");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  ArduinoOTA.setHostname("tally-controller");
  ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA end"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error %u\n", error); });
  ArduinoOTA.begin();

  if (config.protocolEnabled) startProtocol();
  else Serial.println("Protocol disabled; not connecting to switcher.");
}

void loop()
{
  if (config.protocolEnabled && !protocolRunning) startProtocol();
  else if (!config.protocolEnabled && protocolRunning) stopProtocol();

  if (config.protocolEnabled && protocolRunning) {
    if (config.protocol == PROTOCOL_ATEM) atem_loop();
    else if (config.protocol == PROTOCOL_OBS) obs_loop();
    else if (config.protocol == PROTOCOL_VMIX) vmix_loop();
  }
  espnow_loop();
  webserverLoop();
  statusDisplayLoop();
  ArduinoOTA.handle();
  delay(20);
}

void startProtocol() {
  if (protocolRunning || !config.protocolEnabled) return;
  if (config.protocol == PROTOCOL_ATEM) atem_setup();
  else if (config.protocol == PROTOCOL_OBS) obs_setup();
  else if (config.protocol == PROTOCOL_VMIX) vmix_setup();
  protocolRunning = true;
}

void stopProtocol() {
  if (!protocolRunning) return;
  if (config.protocol == PROTOCOL_OBS) obs_stop();
  else if (config.protocol == PROTOCOL_VMIX) vmix_stop();
  protocolRunning = false;
}
