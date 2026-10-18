// host_link.h — CYD/XIAO host <-> C5 UART link: owns the LinkSerial instance + FrameParser, sends
// frames, pings the C5, and runs one scan request/response (carved from the CYD main.cpp,
// docs/mini-build.md §4.1). The link is STRICTLY synchronous (see requestScan()).
#pragma once
#include <Arduino.h>
#include "link_protocol.h"

using namespace link_protocol;

extern HardwareSerial LinkSerial;  // UART1
extern FrameParser    parser;

namespace hostlink {
// Bring up the link UART on the board's pins (RX buffer sized for a full detection burst).
void begin(int rxPin, int txPin);
}

void sendFrame(uint8_t type, const void* payload, uint16_t len);
bool pingC5(uint32_t timeoutMs = 400);
bool requestScan(uint32_t noRespMs = 800, uint32_t timeoutMs = 5000);
