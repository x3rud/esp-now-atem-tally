#pragma once

#include <esp_websocket_client.h>
#include <esp_log.h>
#include <esp32-hal-log.h>

#include "main.h"
#include "memory.h"

void vmix_setup();
void vmix_loop();
void vmix_stop();
