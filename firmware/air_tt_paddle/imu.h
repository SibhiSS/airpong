// MPU-6050 register-level driver + complementary filter.
//
// Why register-level and not a library: the two things Phase 0 proved matter
// (wide ranges verified against a real swing, and a config read-back that
// actually confirms the writes stuck) are exactly the details most MPU6050
// Arduino libraries paper over with their own defaults. Owning the registers
// keeps those guarantees visible instead of buried in a dependency.
#pragma once
#include <stdint.h>

struct ImuSample {
  float roll_deg;    // gravity-referenced, zeroed against imuZero()
  float pitch_deg;
  float wx_dps, wy_dps, wz_dps;   // gyro rate, bias-corrected, NOT zeroed
  bool  fault;        // last I2C transaction failed
};

// Configures the sensor (ranges from config.h), then calibrates accelerometer
// offset and gyro bias. The paddle must be held still during this call —
// exactly like Phase 0's calibrateMPU(), extended to also bias the gyro,
// which the fusion filter needs or it will drift even at rest.
bool imuInit();

// Blocking single read of all 14 IMU registers (accel+temp+gyro), unit
// conversion, complementary fusion. Call at a steady cadence (imuInit's
// caller owns the micros() accumulator) — the filter's dt comes from the
// actual elapsed time between calls, not an assumed constant, so an
// occasional late call degrades gracefully instead of corrupting the filter.
ImuSample imuUpdate();

// Re-zero: the next imuUpdate() reading's roll/pitch becomes the new (0,0).
// This is what OP_REZERO triggers — recentering happens on the device so
// every client gets the same simple "just tilt from here" behavior for free.
void imuZero();
bool imuIsCalibrated();
