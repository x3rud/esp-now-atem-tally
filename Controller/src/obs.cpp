#include "ArduinoJson.h"
#include "espnow.h"
#include "obs.h"

#define MULTILINE(...) #__VA_ARGS__

static const char *TAG = "websocket";
esp_websocket_client_handle_t client;
uint64_t DSKbits = 0;

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

uint64_t bitsFromTags(const char* s) {
  uint64_t bits = 0;
  int len = strlen(s);
  bool boundary = true;
  uint8_t n = 0;

  for (int i=0; i<len; i++) {
    if (s[i] == 'T' && boundary) {
      n = 0;
      i++;
      for (; i<len; i++) {
        if (isdigit(s[i])) {
          n = n*10 + s[i]-'0';
        } else {
          break;
        }
      }
      if (n > 0) bits |= bitn(n);
    } 
    boundary = !isalnum(s[i]);
  }
  return bits;
}

void obs_broadcast_signal(uint64_t bits, uint8_t signal) {
  char op[150] = "";
  for (int i=0; i < 64; i++) {
    if (bits & bitn(i)) {
      sprintf(op, MULTILINE({"op":6,"d":{"requestType":"BroadcastCustomEvent","requestId":"b","requestData":{"eventData":{"type":"tally","from":0,"to":%d,"signal":%u}}}}), i, signal);  
      esp_websocket_client_send_text(client, op, strlen(op), portMAX_DELAY);
    }
  }
}

void obs_request_current_scenes() {
  const char* op1 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentProgramScene\",\"requestId\":\"a\",\"requestData\":{}}}";
  esp_websocket_client_send_text(client, op1, strlen(op1), portMAX_DELAY);
  const char* op2 = "{\"op\":6,\"d\":{\"requestType\":\"GetCurrentPreviewScene\",\"requestId\":\"a\",\"requestData\":{}}}";  
  esp_websocket_client_send_text(client, op2, strlen(op2), portMAX_DELAY);
}

void obs_message_handler(StaticJsonDocument<512> doc) {
  switch ((int)doc["op"]) {
  case 5: {
    if (doc["d"]["eventType"] == "CurrentProgramSceneChanged") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      uint64_t program = programBits | DSKbits;
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"]  == "CurrentPreviewSceneChanged") {
      previewBits = bitsFromTags(doc["d"]["eventData"]["sceneName"]);
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "SceneTransitionStarted") {
      programBits |= previewBits;
      espnow_tally(&programBits, &previewBits);
    }
    else if (doc["d"]["eventType"] == "CustomEvent"
            && doc["d"]["eventData"]["type"] == "tally") {
      uint64_t bits = bitn(doc["d"]["eventData"]["to"].as<int>());
      espnow_signal(doc["d"]["eventData"]["signal"].as<uint8_t>(), &bits);
    }
    else if (doc["d"]["eventType"] == "VendorEvent" && doc["d"]["eventData"]["vendorName"] == "downstream-keyer") {
      DSKbits = bitsFromTags(doc["d"]["eventData"]["eventData"]["new_scene"]);
      uint64_t program = programBits | DSKbits;
      espnow_tally(&program, &previewBits);
		}
    break;
  }
  case 7:
    if (doc["d"]["requestType"] == "GetCurrentProgramScene") {
      // https://github.com/obsproject/obs-websocket/blob/5.3.3/docs/generated/protocol.md#getsceneitemlist
      programBits = bitsFromTags(doc["d"]["responseData"]["currentProgramSceneName"]);
      programBits |= DSKbits;
      espnow_tally(&programBits, &previewBits);
    } else if (doc["d"]["requestType"] == "GetCurrentPreviewScene") {
      previewBits = bitsFromTags(doc["d"]["responseData"]["currentPreviewSceneName"]);
      espnow_tally(&programBits, &previewBits);
    }
    break;
  case 2:
    obs_request_current_scenes();
    break;
  case 0: {
    // Scenes=4
    // Inputs=8
    // Transitions=16
    // SceneItems=128
    // InputShowStateChanged=262144 == preview anywhere in ui
    // InputActiveStateChanged=131072 == program
    // =393216
    const char* op1 = "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":532}}";
    esp_websocket_client_send_text(client, op1, strlen(op1), portMAX_DELAY);
    break;
    }
  }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
  case WEBSOCKET_EVENT_DATA: {
    // Serial.printf("Received opcode=%d\n", data->op_code);
    if (data->op_code == 1) {
      Serial.printf("<%.*s\n", data->data_len, (char *)data->data_ptr);
      StaticJsonDocument<512> doc;  // list of scenes is larger than 512 bytes
      auto error = deserializeJson(doc, data->data_ptr + data->payload_offset, data->payload_len);
      if (error) {
        Serial.print(F("deserializeJson() failed with code "));
        Serial.println(error.c_str());
        return;
      }
      obs_message_handler(doc);
    } else {
      // Serial.printf("Received=%.*s\n", data->data_len, (char *)data->data_ptr);
    }
    break;
  }
  case WEBSOCKET_EVENT_ERROR:
    Serial.println("WS ERROR");
    break;
  case WEBSOCKET_EVENT_CONNECTED:
    Serial.println("WS CONNECTED");
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    Serial.println("WS DISCONNECTED");
    break;
  }
}

void obs_setup() {
  char uri[64];
  sprintf(uri, "ws://%s", inet_ntoa(config.obsIP), config.obsPort);
  Serial.printf("obs_setup %s\n", uri);
  const esp_websocket_client_config_t ws_cfg = {
    .uri = uri,
    .port = config.obsPort,
  };
  obs_stop();
  client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
  esp_websocket_client_start(client);
}

void obs_loop() {
  if (!client || !esp_websocket_client_is_connected(client)) {
    obs_stop();
    obs_setup();
  }
}

void obs_stop() {
  if (client) {
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    client = nullptr;
  }
}
