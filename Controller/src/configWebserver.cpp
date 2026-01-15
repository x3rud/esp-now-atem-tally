#include "configWebserver.h"
#include <Arduino.h>
// #include <WebServer_WT32_ETH01.h>
#include <ETH.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include "atem.h"
#include "obs.h"
#include "espnow.h"
#include "main.h"

static bool eth_connected = false;
WebServer web(80);
static bool fs_ready = false;

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
      break;
    } else if (name == "program") {
      uint64_t programBits = bitsFromCSV(web.arg(i));
      uint64_t previewBits = bitsFromCSV(web.arg("preview"));
      espnow_tally(&programBits, &previewBits);
      break;
    } else if (name == "brightness") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      espnow_brightness(web.arg(i).toInt(), &bits);
      break;
    } else if (name == "camid" && !web.hasArg("mac")) {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      espnow_camid(web.arg(i).toInt(), &bits);
      break;
    } else if (name == "signal") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      long signal = web.arg(i).toInt();
      espnow_signal(signal, &bits);
      if (config.protocol == PROTOCOL_OBS) obs_broadcast_signal(bits, signal);
      break;
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
      break;
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
      break;
    } else if (name == "blink") {
      uint64_t bits = bitsFromCSV(web.arg("i"));
      uint32_t color = parseHexColor(web.arg(i));
      bool enable = !web.hasArg("off");
      espnow_blink(color, enable, &bits);
      break;
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
      break;
    }
  }
  web.send(200, "text/plain", "OK");
  delay(10);
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
    s += "},";
  }
  if (s[s.length()-1] == ',') s.remove(s.length()-1, 1); // remove last ,
  s += "]}";
  web.send(200, "application/json", s);
}

void handleUpdatePage() {
  const char* page = R"(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>OTA Update</title><style>body{font-family:Arial, sans-serif; background:#0f172a; color:#e2e8f0; display:flex; align-items:center; justify-content:center; height:100vh; margin:0;} .card{background:#111827; padding:1.5rem; border-radius:12px; width:90%; max-width:480px; box-shadow:0 20px 50px rgba(0,0,0,0.35);} h1{margin:0 0 1rem 0; font-size:1.4rem;} label{display:block; margin:1rem 0; padding:0.8rem; border:1px dashed #334155; border-radius:10px; cursor:pointer; text-align:center;} input[type=file]{display:none;} button{width:100%; padding:0.9rem; border:none; border-radius:10px; background:#22c55e; color:#0b1727; font-weight:700; cursor:pointer; font-size:1rem;} button:disabled{background:#334155; color:#94a3b8;} .status{margin-top:0.8rem; font-size:0.9rem; min-height:1.2rem;}</style></head><body><div class='card'><h1>OTA Firmware Update</h1><form method='POST' action='/update' enctype='multipart/form-data'><label id='label'><span id='labelText'>Choose firmware (.bin)</span><input type='file' name='firmware' id='file'></label><button id='btn' type='submit' disabled>Upload & Restart</button><div class='status' id='status'></div></form></div><script>const file=document.getElementById('file');const btn=document.getElementById('btn');const text=document.getElementById('labelText');const status=document.getElementById('status');file.addEventListener('change',()=>{if(file.files.length){text.textContent=file.files[0].name;btn.disabled=false;status.textContent='';}});document.querySelector('form').addEventListener('submit',(e)=>{if(!file.files.length){e.preventDefault();return;}status.textContent='Uploading...';btn.disabled=true;});</script></body></html>)";
  web.send(200, "text/html", page);
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
}

void webserverLoop() {
  web.handleClient();
}
