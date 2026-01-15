#include <ETH.h>
#include "espnow.h"
#include "vmix.h"

#define TALLY_UPDATE_EACH 2000
#define MULTILINE(...) #__VA_ARGS__

WiFiClient tcpclient;
char buffer[1024];

inline uint64_t bitn(uint8_t n) {
  return (uint64_t)1 << (n-1);
}

void vmix_setup() {
  Serial.print("VMIX IP:");
  Serial.println(IPAddress(config.vmixIP).toString());
  if (tcpclient.connect(config.vmixIP, config.vmixPort)) {
    Serial.print("SUBSCRIBE TALLY\r\n");
    tcpclient.print("SUBSCRIBE TALLY\r\n");
  }
}

void vmix_loop() {
  tcpclient.readBytesUntil('\n', buffer, 1024);
  if (strncmp(buffer, "TALLY OK ", 9) == 0) {
    programBits = 0;
    previewBits = 0;
    int i = 1;
    char *b = buffer+9-i;
    while (true) {
      if      (b[i] == '0') continue;
      else if (b[i] == '1') programBits |= bitn(i+1);
      else if (b[i] == '2') previewBits |= bitn(i+1);
      else if (b[i] == '3') {
        programBits |= bitn(i+1);
        previewBits |= bitn(i+1);
      }
      else break;
      i++;
    }
    espnow_tally(&programBits, &previewBits);
  } else if (strncmp(buffer, "SUBSCRIBE OK TALLY", 18) == 0) {
    Serial.println("SUBSCRIBE OK TALLY");
  }

  if (!tcpclient.connected()) {
    vmix_setup();
  }
}

void vmix_stop() {
  if (tcpclient.connected()) {
    tcpclient.stop();
  }
}
