#include "configWebserver.h"
#include <Arduino.h>
// #include <WebServer_WT32_ETH01.h>
#include <ETH.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#ifndef DISABLE_WS
#include <WebSocketsServer.h>
#endif
#include "atem.h"
#include "obs.h"
#include "espnow.h"
#include "main.h"

static bool eth_connected = false;
WebServer web(80);
static bool fs_ready = false;
#ifndef DISABLE_WS
WebSocketsServer wsServer(81);
#endif

IPAddress asIp(uint32_t i) {
  return IPAddress(i);
}

String contentTypeForPath(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg")) return "image/svg+xml";
  return "text/plain";
}

bool handleFileRead(const String& path) {
  if (!fs_ready) return false;
  String localPath = path;
  if (localPath.endsWith("/")) localPath += "index.html";
  if (!SPIFFS.exists(localPath)) return false;

  File file = SPIFFS.open(localPath, "r");
  if (!file) return false;
  web.streamFile(file, contentTypeForPath(localPath));
  file.close();
  return true;
}

void handleRoot() {
  if (handleFileRead("/index.html")) return;
  web.send(500, "text/plain", "index.html not found");
}

void handleConfigJson() {
  String s = "{";
  s += "\"protocol\":";
  s += config.protocol;
  s += ",\"connect\":";
  s += (config.protocolEnabled ? 1 : 0);
  s += ",\"atemip\":\"";
  s += asIp(config.atemIP).toString();
  s += "\",\"obsip\":\"";
  s += asIp(config.obsIP).toString();
  s += "\",\"obsport\":";
  s += config.obsPort;
  s += ",\"vmixip\":\"";
  s += asIp(config.vmixIP).toString();
  s += "\",\"vmixport\":";
  s += config.vmixPort;
  s += ",\"tallies\":";
  // embed current tallies for faster load
  {
    String t = "[";
    espnow_tally_info_t *tallies = espnow_tallies();
    unsigned long now = millis();
    for (int i=0; i<MAX_TALLY_COUNT; i++) {
      if (tallies[i].id == 0) continue;
      char macbuf[18];
      sprintf(macbuf, "%02X:%02X:%02X:%02X:%02X:%02X",
              tallies[i].mac_addr[0], tallies[i].mac_addr[1], tallies[i].mac_addr[2],
              tallies[i].mac_addr[3], tallies[i].mac_addr[4], tallies[i].mac_addr[5]);
      t += "{\"id\":";
      t += tallies[i].id;
      t += ",\"seen\":";
      t += ((now - tallies[i].last_seen) / 1000);
      t += ",\"mac\":\"";
      t += macbuf;
      t += "\",\"name\":\"";
      t += tallies[i].name;
      t += "\",\"signal\":";
      t += tallies[i].signal;
      t += ",\"rgbBrightness\":";
      t += tallies[i].rgbBrightness;
      t += ",\"statusBrightness\":";
      t += tallies[i].statusBrightness;
      t += "},";
    }
    if (t[t.length()-1] == ',') t.remove(t.length()-1, 1);
    t += "]";
    s += t;
  }
  s += "}";
  web.send(200, "application/json", s);
}

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

uint64_t bitsFromCSV(String s) {
  uint64_t bits = 0;
  int number = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (isdigit(s[i])) {
      number = 10*number + s[i] - '0';
    } else if (number > 0) {
      bits |= bitn(number);
      number = 0;
    }
  }
  if (number > 0) bits |= bitn(number);
  return bits;
}

uint32_t parseHexColor(String s) {
  uint32_t color = 0;
  for (int i=0; i < s.length(); i++) {
    unsigned char c = s[i];
    color = color << 4;
    if      (c >= '0' && c <= '9') color += c - '0';
    else if (c >= 'A' && c <= 'F') color += c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') color += c - 'a' + 10;
    // else undefined
  }
  return color;
}

void handleSet() {
  String name;
  IPAddress ip;
  bool configUpdated = false;
  bool connectionChanged = false;
  for (int i=0; i<web.args(); i++) {
    name = web.argName(i);
    if (name == "color") {
      uint32_t color = parseHexColor(web.arg(i));
      uint64_t bits = bitsFromCSV(web.arg("i"));
      espnow_color(color, &bits);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "program") {
      uint64_t programBits = bitsFromCSV(web.arg(i));
      uint64_t previewBits = bitsFromCSV(web.arg("preview"));
      espnow_tally(&programBits, &previewBits);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "brightness" && !web.hasArg("mac")) {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      espnow_brightness(web.arg(i).toInt(), &bits);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "camid" && !web.hasArg("mac")) {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      espnow_camid(web.arg(i).toInt(), &bits);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "signal") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      long signal = web.arg(i).toInt();
      espnow_signal(signal, &bits);
      if (config.protocol == PROTOCOL_OBS) obs_broadcast_signal(bits, signal);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "protocol") {
      config.protocol = (switcher_protocol) web.arg(i).toInt();
      if (config.protocol != PROTOCOL_ATEM && config.protocol != PROTOCOL_OBS && config.protocol != PROTOCOL_VMIX) return;
      configUpdated = true;
    } else if (name == "connect") {
      config.protocolEnabled = web.arg(i).toInt() != 0;
      connectionChanged = true;
    } else if (name == "atemip") {
      ip.fromString(web.arg(i));
      config.atemIP = (uint32_t) ip;
      if (config.atemIP == 0) return;
      configUpdated = true;
    } else if (name == "obsip") {
      ip.fromString(web.arg(i));
      config.obsIP = (uint32_t) ip;
      if (config.obsIP == 0) return;
      configUpdated = true;
    } else if (name == "obsport") {
      config.obsPort = web.arg(i).toInt();
      if (config.obsPort == 0) return;
      configUpdated = true;
    } else if (name == "vmixip") {
      ip.fromString(web.arg(i));
      config.vmixIP = (uint32_t) ip;
      if (config.vmixIP == 0) return;
      configUpdated = true;
    } else if (name == "vmixport") {
      config.vmixPort = web.arg(i).toInt();
      if (config.vmixPort == 0) return;
      configUpdated = true;
    } else if (name == "name") {
      if (web.hasArg("mac")) {
        uint8_t mac[6];
        String macStr = web.arg("mac");
        macStr.toUpperCase();
        int idx = 0, val = 0, nib = 0;
        for (size_t k=0; k<macStr.length() && idx<6; k++) {
          char c = macStr[k];
          if (c == ':' || c == '-') continue;
          if (c >= '0' && c <= '9') val = (val << 4) + c - '0';
          else if (c >= 'A' && c <= 'F') val = (val << 4) + c - 'A' + 10;
          nib++;
          if (nib == 2) { mac[idx++] = val; val = 0; nib = 0; }
        }
      if (idx == 6) espnow_set_name_mac(web.arg(i), mac);
      } else {
        uint64_t bits = bitsFromCSV(web.arg("i"));
        espnow_set_name(web.arg(i), &bits);
      }
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "identify") {
      uint8_t seconds = web.hasArg("seconds") ? web.arg("seconds").toInt() : 5;
      if (web.hasArg("mac")) {
        uint8_t mac[6];
        String macStr = web.arg("mac");
        macStr.toUpperCase();
        int idx = 0;
        int val = 0;
        int nib = 0;
        for (size_t k=0; k<macStr.length() && idx<6; k++) {
          char c = macStr[k];
          if (c == ':' || c == '-') continue;
          if (c >= '0' && c <= '9') val = (val << 4) + c - '0';
          else if (c >= 'A' && c <= 'F') val = (val << 4) + c - 'A' + 10;
          nib++;
          if (nib == 2) { mac[idx++] = val; val = 0; nib = 0; }
        }
        if (idx == 6) espnow_identify_mac(mac, seconds);
      } else {
        uint64_t bits = bitsFromCSV(web.arg("i"));
        espnow_identify(&bits, seconds);
      }
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "blink") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      uint32_t color = parseHexColor(web.arg(i));
      bool enable = !web.hasArg("off");
      espnow_blink(color, enable, &bits);
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "camid" && web.hasArg("mac")) {
      uint8_t mac[6];
      String macStr = web.arg("mac");
      macStr.toUpperCase();
      int idx = 0, val = 0, nib = 0;
      for (size_t k=0; k<macStr.length() && idx<6; k++) {
        char c = macStr[k];
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9') val = (val << 4) + c - '0';
        else if (c >= 'A' && c <= 'F') val = (val << 4) + c - 'A' + 10;
        nib++;
        if (nib == 2) { mac[idx++] = val; val = 0; nib = 0; }
      }
      if (idx == 6) {
        uint8_t newId = web.arg(i).toInt();
        espnow_set_camid_mac(newId, mac);
      }
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "brightness" && web.hasArg("mac")) {
      uint8_t mac[6];
      String macStr = web.arg("mac");
      macStr.toUpperCase();
      int idx = 0, val = 0, nib = 0;
      for (size_t k=0; k<macStr.length() && idx<6; k++) {
        char c = macStr[k];
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9') val = (val << 4) + c - '0';
        else if (c >= 'A' && c <= 'F') val = (val << 4) + c - 'A' + 10;
        nib++;
        if (nib == 2) { mac[idx++] = val; val = 0; nib = 0; }
      }
      if (idx == 6) {
        uint8_t b = web.arg(i).toInt();
        espnow_brightness_mac(b, mac);
      }
      web.send(200, "text/plain", "OK");
      return;
    } else if (name == "statusbrightness" && web.hasArg("mac")) {
      uint8_t mac[6];
      String macStr = web.arg("mac");
      macStr.toUpperCase();
      int idx = 0, val = 0, nib = 0;
      for (size_t k=0; k<macStr.length() && idx<6; k++) {
        char c = macStr[k];
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9') val = (val << 4) + c - '0';
        else if (c >= 'A' && c <= 'F') val = (val << 4) + c - 'A' + 10;
        nib++;
        if (nib == 2) { mac[idx++] = val; val = 0; nib = 0; }
      }
      if (idx == 6) {
        uint8_t b = web.arg(i).toInt();
        espnow_status_brightness(b, mac);
      }
      web.send(200, "text/plain", "OK");
      return;
    }
  }
  web.send(200, "text/plain", "OK");
  // immediate return; no delay
  if (configUpdated) {
    writeConfig();
    ESP.restart();
  } else if (connectionChanged) {
    writeConfig();
  }
}

void handleTally() {
  web.send(200, "application/json", "{\"program\":"+String(programBits)+",\"preview\":"+String(previewBits)+"}");
}

void handleSeen() {
  String s = "{\"tallies\":[";
  espnow_tally_info_t *tallies = espnow_tallies();
  unsigned long now = millis();
  for (int i=0; i<64; i++) {
    if (tallies[i].id == 0) continue;
    char macbuf[18];
    sprintf(macbuf, "%02X:%02X:%02X:%02X:%02X:%02X",
            tallies[i].mac_addr[0], tallies[i].mac_addr[1], tallies[i].mac_addr[2],
            tallies[i].mac_addr[3], tallies[i].mac_addr[4], tallies[i].mac_addr[5]);
    s += "{\"id\":";
    s += tallies[i].id;
    s += ", \"seen\":";
    s += ((now - tallies[i].last_seen) / 1000);
    s += ", \"mac\":\"";
    s += macbuf;
    s += "\", \"name\":\"";
    s += tallies[i].name;
    s += "\", \"signal\":";
    s += tallies[i].signal;
    s += ", \"rgbBrightness\":";
    s += tallies[i].rgbBrightness;
    s += ", \"statusBrightness\":";
    s += tallies[i].statusBrightness;
    s += "},";
  }
  if (s[s.length()-1] == ',') s.remove(s.length()-1, 1); // remove last ,
  s += "]}";
  web.send(200, "application/json", s);
}

String buildTallyPayload() {
  String s = "{\"program\":";
  s += String(programBits);
  s += ",\"preview\":";
  s += String(previewBits);
  s += "}";
  return s;
}

String buildDevicesPayload() {
  String s = "{\"tallies\":[";
  espnow_tally_info_t *tallies = espnow_tallies();
  unsigned long now = millis();
  for (int i=0; i<64; i++) {
    if (tallies[i].id == 0) continue;
    char macbuf[18];
    sprintf(macbuf, "%02X:%02X:%02X:%02X:%02X:%02X",
            tallies[i].mac_addr[0], tallies[i].mac_addr[1], tallies[i].mac_addr[2],
            tallies[i].mac_addr[3], tallies[i].mac_addr[4], tallies[i].mac_addr[5]);
    s += "{\"id\":";
    s += tallies[i].id;
    s += ",\"seen\":";
    s += ((now - tallies[i].last_seen) / 1000);
    s += ",\"mac\":\"";
    s += macbuf;
    s += "\",\"name\":\"";
    s += tallies[i].name;
    s += "\",\"signal\":";
    s += tallies[i].signal;
    s += ",\"rgbBrightness\":";
    s += tallies[i].rgbBrightness;
    s += ",\"statusBrightness\":";
    s += tallies[i].statusBrightness;
    s += "},";
  }
  if (s[s.length()-1] == ',') s.remove(s.length()-1, 1); // remove last ,
  s += "]}";
  return s;
}

void broadcastState() {
#ifdef DISABLE_WS
  return;
#else
  if (wsServer.connectedClients() == 0) return;
  String t = buildTallyPayload();
  String d = buildDevicesPayload();
  wsServer.broadcastTXT("{\"type\":\"tally\",\"data\":" + t + "}");
  wsServer.broadcastTXT("{\"type\":\"devices\",\"data\":" + d + "}");
#endif
}

void handleUpdatePage() {
  if (handleFileRead("/ota.html")) return;
  web.send(500, "text/plain", "ota.html not found");
}

void handleUpdateUpload() {
  HTTPUpload &upload = web.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleUpdateResult() {
  if (Update.hasError()) {
    web.send(500, "text/plain", "Update failed");
  } else {
    web.send(200, "text/plain", "Update OK. Rebooting...");
    delay(200);
    ESP.restart();
  }
}

void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      //set eth hostname here
      ETH.setHostname("esp32-ethernet");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH MAC: ");
      Serial.print(ETH.macAddress());
      Serial.print(", IPv4: ");
      Serial.print(ETH.localIP());
      if (ETH.fullDuplex()) {
        Serial.print(", FULL_DUPLEX");
      }
      Serial.print(", ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;
    default:
      break;
  }
}

void setupWebserver() {
  Serial.print("Starting HTTP server on ");
  Serial.print(ARDUINO_BOARD);
  #ifdef WEBSERVER_WT32_ETH01_VERSION
  Serial.print(" with ");
  Serial.println(SHIELD_TYPE);
  Serial.println(WEBSERVER_WT32_ETH01_VERSION);
  // To be called before ETH.begin()
  WT32_ETH01_onEvent();
  // Initialize the Ethernet connection
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER);
  WT32_ETH01_waitForConnect();
  #else
  WiFi.onEvent(WiFiEvent);
  ETH.begin();
  #endif
  fs_ready = SPIFFS.begin(true);
  if (!fs_ready) Serial.println("SPIFFS mount failed");
  web.on("/tally", handleTally);
  web.on("/set", handleSet);
  web.on("/seen", handleSeen);
  web.on("/config", handleConfigJson);
  web.on("/update", HTTP_GET, handleUpdatePage);
  web.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  web.on("/", handleRoot);
  web.onNotFound([]() {
    if (handleFileRead(web.uri())) return;
    web.send(404, "text/plain", "Not found");
  });
  web.begin();
#ifndef DISABLE_WS
  wsServer.begin();
  wsServer.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
      wsServer.sendTXT(num, "{\"type\":\"tally\",\"data\":" + buildTallyPayload() + "}");
      wsServer.sendTXT(num, "{\"type\":\"devices\",\"data\":" + buildDevicesPayload() + "}");
    }
  });
#endif
}

void webserverLoop() {
  web.handleClient();
#ifndef DISABLE_WS
  wsServer.loop();
#endif
}
