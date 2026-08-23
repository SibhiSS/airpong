// Air TT paddle — all tuning constants, pins, and network config in one place.
// Nothing below should require touching imu.cpp / net.cpp to change.
#pragma once

// ---------------- I2C / MPU-6050 ----------------
// Do not hardcode GPIO numbers. Every ESP32 board definition declares its own
// default I2C pins as the SDA/SCL macros, and the boot banner prints the
// resolved GPIO numbers so the wiring can be checked against hardware instead
// of guessed from a board's silkscreen. See imu.cpp / docs/WIRING.md.
#ifndef PIN_SDA_OVERRIDE
  #define PIN_SDA_OVERRIDE -1
  #define PIN_SCL_OVERRIDE -1
#endif
#define I2C_CLOCK_HZ 400000

// Ranges verified against a real swing in Phase 0 (firmware/phase0_bringup):
// peak 8.07 g / 869 deg/s with zero clipping. The reference project's default
// +/-2 g / +/-250 dps ranges clip instantly on any real forehand.
#define IMU_AFS_SEL 2   // 2 -> +/-8 g,     4096   LSB/g
#define IMU_FS_SEL  3   // 3 -> +/-2000 dps, 16.4  LSB/(deg/s)
#define IMU_DLPF    3   // ~44 Hz accel / 42 Hz gyro
#define IMU_SMPLRT  4   // 1000/(1+4) = 200 Hz base sample rate

static const float ACCEL_LSB_PER_G  = 4096.0f;
static const float GYRO_LSB_PER_DPS = 16.4f;

// Complementary filter: high ALPHA trusts the (drift-free but noisy and
// swing-corrupted) gyro integration; the small remainder pulls slowly toward
// the accelerometer's gravity-referenced estimate to cancel long-term drift.
static const float FUSION_ALPHA = 0.98f;

// ---------------- timing ----------------
// Sampling and sending are deliberately decoupled (PLAN.md section 4): the
// fusion filter needs a steady, trustworthy dt, so it is driven off a fixed
// micros() accumulator rather than "however often loop() gets back around".
static const uint32_t SAMPLE_INTERVAL_US   = 5000;   // 200 Hz
static const uint16_t SEND_INTERVAL_MS_DEFAULT = 10; // 100 Hz
static const uint16_t SEND_INTERVAL_MS_MIN = 5;      // 200 Hz cap
static const uint16_t SEND_INTERVAL_MS_MAX = 100;    // 10 Hz floor

// ---------------- WiFi ----------------
// The AP is always raised regardless of STA outcome, so the paddle works with
// zero infrastructure — no router, no phone hotspot. Leave STA_SSID empty to
// skip the STA attempt entirely.
#define AP_SSID_PREFIX   "AirTT-"      // suffixed with 4 hex digits of the MAC
#define AP_PASSWORD      "airtt2026"   // WPA2 needs >=8 chars; change if you like
#define STA_SSID         ""            // set to join an existing router/hotspot
#define STA_PASSWORD     ""
#define STA_CONNECT_TIMEOUT_MS 8000

// ---------------- drift correction ----------------
// A MEMS gyro's zero-rate output is never exactly zero and moves as the part
// warms up, so the one-shot calibration at boot goes stale within minutes.
// The fused angle then leans steadily to one side and the on-screen paddle
// "drags" toward an edge, which is what makes one side of the table hard to
// reach. Two continuous corrections, both only active while the paddle is
// genuinely still:
//
//   1. bias tracking  - if it is not moving, any rate the gyro reports IS
//                       bias, so fold a little of it back into the offset.
//   2. auto re-centre - pull the zero point gently toward the current pose,
//                       slowly enough that deliberately holding an angle for
//                       a moment does not steal your aim.
static const float STILL_GYRO_DPS = 6.0f;    // all axes under this => not rotating
static const float STILL_ACC_TOL  = 0.08f;   // |a| within this of 1g => not accelerating
static const uint32_t STILL_MS_BEFORE_RECENTRE = 1200;
static const float GYRO_BIAS_TRACK = 0.0020f;  // per-sample, ~2.5s time constant at 200Hz
static const float AUTO_ZERO_RATE  = 0.0015f;  // per-sample, ~3.3s time constant at 200Hz

// ---------------- swing detection ----------------
// Phase 0 measured ~2 dps at rest and 869 dps peak on a hard real swing —
// these sit deliberately between "aiming the paddle by tilting" and "an
// actual swing", with a gap between rise/fall (hysteresis) so one swing
// reads as one swing instead of chattering at the boundary. These are
// starting points, not final: retune against your own swings with
// tools/scope.html (that's what its swing readout is for).
static const float SWING_RISE_DPS = 170.0f;
static const float SWING_FALL_DPS = 70.0f;

// ---------------- wire protocol ----------------
// See docs/PROTOCOL.md for the byte-level layout.
static const uint8_t FRAME_MAGIC = 0xA7;
static const uint8_t FLAG_SWING_ACTIVE = 1 << 0;
static const uint8_t FLAG_CALIBRATED   = 1 << 1;
static const uint8_t FLAG_IMU_FAULT    = 1 << 2;
static const uint8_t FLAG_BUTTON       = 1 << 3;

static const uint8_t OP_PING    = 0x01;
static const uint8_t OP_REZERO  = 0x02;
static const uint8_t OP_SETRATE = 0x03;
