/*
 * Air Table Tennis — paddle firmware, Phase 1: telemetry pipeline.
 *
 * Samples the MPU-6050 at a fixed 200 Hz, fuses it into drift-free,
 * swing-proof roll/pitch (imu.cpp), and streams it over WebSocket at 100 Hz
 * as a compact binary frame (net.cpp, docs/PROTOCOL.md) to whatever browser
 * is watching — tools/scope.html today, the game itself from Phase 2 on.
 *
 * Deliberately NOT in this file: swing detection (Phase 3) and serving the
 * game from LittleFS (Phase 4). This phase only has to prove the telemetry
 * link is fast and steady — everything downstream depends on that being true
 * before it's worth building on top of.
 */
#include "config.h"
#include "imu.h"
#include "net.h"
#include "swing.h"
#include <math.h>

static uint32_t nextSampleUs = 0;
static uint32_t lastSendMs = 0;
static ImuSample latest{};

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n### Air TT paddle — Phase 1 ###");

  if (!imuInit()) {
    Serial.println("!! IMU init failed. Check wiring (docs/WIRING.md) and reset.");
    // Keep going rather than halt: the WiFi telemetry link is still worth
    // bringing up so the fault is visible in the stream (FLAG_IMU_FAULT)
    // instead of a silent, un-debuggable brick.
  }

  netInit();

  Serial.println("\nReady. Commands: 'z' re-zero, 'r' recalibrate IMU.");
  Serial.println("Open tools/scope.html, source = WebSocket, connect to the printed AP.\n");

  nextSampleUs = micros();
  lastSendMs = millis();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'z') imuZero();
    else if (c == 'r') imuInit();
  }

  // Fixed-cadence sampling off a micros() accumulator, not delay() — the
  // complementary filter's dt has to be trustworthy (see imu.cpp), and that
  // starts with a sample clock that doesn't jitter with whatever else this
  // loop iteration did.
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) >= 0) {
    nextSampleUs += SAMPLE_INTERVAL_US;
    if ((int32_t)(micros() - nextSampleUs) > (int32_t)(SAMPLE_INTERVAL_US * 4)) {
      nextSampleUs = micros() + SAMPLE_INTERVAL_US;  // resync after a big stall
    }
    latest = imuUpdate();
    float gyroMag = sqrtf(latest.wx_dps * latest.wx_dps + latest.wy_dps * latest.wy_dps + latest.wz_dps * latest.wz_dps);
    swingUpdate(gyroMag, millis());
  }

  uint32_t nowMs = millis();
  if (nowMs - lastSendMs >= netGetSendIntervalMs()) {
    lastSendMs = nowMs;
    netSendTelemetry(latest);
  }
}
