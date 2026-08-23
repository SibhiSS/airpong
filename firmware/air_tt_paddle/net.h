// WiFi (AP+STA) and the WebSocket telemetry link.
//
// Async, not the reference project's blocking WebSocketsServer + webSocket.loop()
// in the main loop: the 200 Hz IMU sampling accumulator (config.h,
// SAMPLE_INTERVAL_US) needs `loop()` back quickly and on a predictable cadence,
// and an async server serves the network off its own task instead of stealing
// unpredictable slices of loop() time. This is also the server Phase 4 will
// reuse to host the gzipped game from LittleFS, so it is worth having be right
// now rather than swapped out later.
#pragma once
#include "imu.h"

// Starts the AP (always) and STA (if config.h STA_SSID is set), then the
// HTTP + WebSocket server. Blocks for at most STA_CONNECT_TIMEOUT_MS.
void netInit();

// Encodes one 20-byte frame (docs/PROTOCOL.md) and broadcasts it to every
// connected WebSocket client. Owns its own sequence counter.
void netSendTelemetry(const ImuSample &s);

// Current send-loop period, adjustable at runtime via the OP_SETRATE opcode.
uint16_t netGetSendIntervalMs();
