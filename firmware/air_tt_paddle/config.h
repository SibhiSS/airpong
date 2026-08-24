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

// Indoors, a small room is the best possible RF environment: the tablet is a
// couple of metres away and every wall bounces a second copy of the signal
// around your body. Outdoors there are no reflections at all, so the only path
// left is the direct one — and that path runs through the arm and torso of the
// person holding the paddle, which costs 10-20 dB at 2.4 GHz. Same hardware,
// far worse link. Everything below is about not making that worse than it has
// to be.
//
// Channel is pinned rather than left at the default so it can be moved off a
// busy one without hunting through the API; 1 / 6 / 11 are the only
// non-overlapping choices.
#define AP_CHANNEL       1
// Bounded so a phone that walks out of range and silently dies does not sit in
// the client list burning airtime on retransmits while its replacement
// connects. The game needs exactly one screen; the spare slot is for having
// /scope open alongside it.
#define WS_MAX_CLIENTS   2

// ---------------- link backpressure ----------------
// The failure this exists to prevent: telemetry was previously broadcast every
// SEND_INTERVAL_MS regardless of whether the last frame had actually left the
// device. That is fine on a strong link, where the queue drains faster than it
// fills. On a weak one it is a latency pump — frames pile up in the TCP send
// queue, and because WebSocket runs over TCP the client cannot skip ahead to
// the newest one, it has to be handed every stale frame first. The paddle then
// renders where your hand was a second ago and the lag grows the longer you
// play. That is a queue, not a slow link, and no amount of signal fixes it.
//
// The rule instead: never queue an orientation behind an older one. If the
// previous frame has not drained, drop this one — the next sample supersedes it
// anyway. Latency then stays bounded no matter how bad the link gets, and the
// stream degrades in update rate instead, which the client's extrapolation is
// there to cover.
static const uint8_t  BACKPRESSURE_SKIPS_BEFORE_BACKOFF = 8;
static const uint16_t SEND_INTERVAL_BACKOFF_MS = 5;    // added per backoff step
static const uint16_t SEND_INTERVAL_CEILING_MS = 33;   // ~30 Hz, the slowest we go
static const uint32_t SEND_RATE_RECOVER_MS = 2000;     // clean for this long => speed back up

// ---------------- link diagnostics ----------------
// Sent alongside telemetry so "it lags outside" can be resolved on the field
// instead of guessed at afterwards: RSSI separates a weak link from a busy one,
// the drop counter separates a weak link from a queue, and the IMU temperature
// separates both from the sensor cooking in the sun.
static const uint16_t STAT_INTERVAL_MS = 500;

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

// The deadlock the above has on its own, and why sunlight is what triggers it:
// a MEMS gyro's zero-rate output moves with die temperature, and a paddle left
// in direct sun goes from a ~25 C room to 50-60 C in minutes. Once that shift
// pushes the bias-corrected rate past STILL_GYRO_DPS, the paddle *never* reads
// as still again — so the bias tracker that exists precisely to correct that
// shift stops running, and the error it would have removed is now permanent.
// It cannot recover on its own. The one-shot boot calibration is also, by
// then, a measurement of a completely different sensor temperature.
//
// The way out is a rest detector that does not consult the gyro at all: if
// gravity's direction has not moved, the paddle is not rotating, whatever the
// gyro claims. Trusting that lets the bias tracker run again and unwind the
// offset. It runs slower than the normal path because the accelerometer cannot
// see rotation about the gravity axis, so a long steady yaw would slowly be
// absorbed as bias — harmless here (yaw is deliberately unused, see
// docs/PROTOCOL.md) but worth not doing quickly.
static const float ACC_REST_FAST    = 0.10f;   // fast EMA on the accel-derived angle
static const float ACC_REST_TRACK   = 0.01f;   // ... and the slow one it is compared against
static const float ACC_REST_TOL_DEG = 1.5f;    // fast and slow within this => not rotating
static const float GYRO_BIAS_RECOVER = 0.0004f;  // ~12s time constant at 200Hz

// ---------------- thermal ----------------
// The MPU-6050's temperature register was already being read as part of the
// 14-byte burst and thrown away. It costs nothing to keep, and it is the
// difference between "the paddle drifts outdoors" and "the paddle is at 54 C,
// 29 C above where it was calibrated, of course it drifts".
static const float TEMP_DRIFT_WARN_C = 8.0f;     // warn once past this much change
static const uint32_t TEMP_WARN_INTERVAL_MS = 30000;

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
static const uint8_t OP_STAT    = 0x04;   // server -> client only, see docs/PROTOCOL.md
