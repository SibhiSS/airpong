#include "imu.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

static const uint8_t REG_SMPLRT_DIV   = 0x19;
static const uint8_t REG_CONFIG       = 0x1A;
static const uint8_t REG_GYRO_CONFIG  = 0x1B;
static const uint8_t REG_ACCEL_CONFIG = 0x1C;
static const uint8_t REG_ACCEL_XOUT_H = 0x3B;
static const uint8_t REG_PWR_MGMT_1   = 0x6B;
static const uint8_t REG_WHO_AM_I     = 0x75;

static uint8_t mpuAddr = 0x68;
static bool    calibrated = false;

static float offAx = 0, offAy = 0, offAz = 0;   // accel offsets, g
static float offGx = 0, offGy = 0, offGz = 0;   // gyro bias, dps

static float rollF = 0, pitchF = 0;             // fused, unzeroed
static float zeroRoll = 0, zeroPitch = 0;
static uint32_t lastUpdateUs = 0;
static bool firstSample = true;
static uint32_t stillSinceMs = 0;   // 0 = currently moving

// Accelerometer-only rest detection, which is what breaks the thermal deadlock
// described in config.h. Two averages of the gravity-derived angle rather than
// one: a fast one that tracks where the paddle is now, and a slow one that
// tracks where it has been. Comparing the two answers "is it rotating?" while
// comparing a raw sample against an average would mostly have answered "is the
// accelerometer noisy?" — at ~0.6 deg RMS of sample noise against a 1.5 deg
// threshold, that is a couple of false trips a second, each one resetting the
// stillness timer that the auto-recentre needs 1.2 s of.

static float tempC = 0;             // die temperature, updated every sample
static float calibTempC = 0;        // ... and what it was when calibrate() ran
static uint32_t lastTempWarnMs = 0;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddr, (uint8_t)n, (uint8_t)true) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Same technique as Phase 0's calibrateMPU, extended to bias the gyro too.
// Accel calibration removes the mount's own tilt error; gyro calibration
// removes the fixed offset every MEMS gyro has, which integrates into a
// steadily growing lean if left uncorrected — invisible until you notice the
// paddle "leaking" sideways over 30 seconds of holding it still.
static void calibrate() {
  Serial.println("--- IMU calibration: hold the paddle stationary and flat ---");
  const int N = 300;
  long sax = 0, say = 0, saz = 0, sgx = 0, sgy = 0, sgz = 0, st = 0;
  int got = 0;

  for (int i = 0; i < N; i++) {
    uint8_t b[14];
    if (rd(REG_ACCEL_XOUT_H, b, 14)) {
      sax += (int16_t)(b[0] << 8 | b[1]);
      say += (int16_t)(b[2] << 8 | b[3]);
      saz += (int16_t)(b[4] << 8 | b[5]);
      st  += (int16_t)(b[6] << 8 | b[7]);
      sgx += (int16_t)(b[8] << 8 | b[9]);
      sgy += (int16_t)(b[10] << 8 | b[11]);
      sgz += (int16_t)(b[12] << 8 | b[13]);
      got++;
    }
    delay(3);
  }

  offAx = (sax / (float)N) / ACCEL_LSB_PER_G;
  offAy = (say / (float)N) / ACCEL_LSB_PER_G;
  // Z carries +1g at rest; subtract that before treating it as an offset.
  offAz = (saz / (float)N) / ACCEL_LSB_PER_G - 1.0f;
  offGx = (sgx / (float)N) / GYRO_LSB_PER_DPS;
  offGy = (sgy / (float)N) / GYRO_LSB_PER_DPS;
  offGz = (sgz / (float)N) / GYRO_LSB_PER_DPS;

  // These offsets are only valid at the temperature they were measured at, so
  // record it. Everything downstream that wonders whether the calibration has
  // gone stale can then compare against a number instead of guessing.
  calibTempC = got ? ((st / (float)got) / 340.0f + 36.53f) : 0.0f;
  tempC = calibTempC;

  Serial.printf("calibration done. accel off (g): %.3f,%.3f,%.3f  gyro bias (dps): %.2f,%.2f,%.2f  @ %.1f C\n",
                offAx, offAy, offAz, offGx, offGy, offGz, calibTempC);
  Serial.println("   (calibrate in the shade if you are about to play in the sun — "
                 "these offsets move with die temperature.)");
}

bool imuInit() {
  int sda = (PIN_SDA_OVERRIDE >= 0) ? PIN_SDA_OVERRIDE : SDA;
  int scl = (PIN_SCL_OVERRIDE >= 0) ? PIN_SCL_OVERRIDE : SCL;
  Wire.begin(sda, scl, I2C_CLOCK_HZ);
  delay(100);
  Serial.printf("I2C pins: SDA=GPIO%d SCL=GPIO%d\n", sda, scl);

  uint8_t who = 0;
  for (uint8_t addr : {(uint8_t)0x68, (uint8_t)0x69}) {
    mpuAddr = addr;
    if (rd(REG_WHO_AM_I, &who, 1)) break;
    who = 0;
  }
  if (who == 0) { Serial.println("MPU-6050 not responding."); return false; }
  Serial.printf("MPU at 0x%02X, WHO_AM_I=0x%02X\n", mpuAddr, who);

  if (!wr(REG_PWR_MGMT_1, 0x01)) return false;   // PLL, X-gyro reference
  delay(50);
  if (!wr(REG_CONFIG, IMU_DLPF)) return false;
  if (!wr(REG_SMPLRT_DIV, IMU_SMPLRT)) return false;
  if (!wr(REG_GYRO_CONFIG,  (uint8_t)(IMU_FS_SEL  << 3))) return false;
  if (!wr(REG_ACCEL_CONFIG, (uint8_t)(IMU_AFS_SEL << 3))) return false;
  delay(50);

  uint8_t v[4] = {0, 0, 0, 0};
  if (!rd(REG_SMPLRT_DIV, v, 4)) { Serial.println("config read-back failed"); return false; }
  bool match = (v[0] == IMU_SMPLRT) && (v[1] == IMU_DLPF) &&
               (((v[2] >> 3) & 3) == IMU_FS_SEL) && (((v[3] >> 3) & 3) == IMU_AFS_SEL);
  if (!match) { Serial.println("!! MPU config mismatch — writes did not stick"); return false; }
  Serial.println("MPU config verified OK");

  calibrate();
  lastUpdateUs = micros();
  firstSample = true;
  stillSinceMs = 0;
  accAvgSeeded = false;
  lastTempWarnMs = 0;
  return true;
}

ImuSample imuUpdate() {
  ImuSample s{};
  uint8_t b[14];

  uint32_t now = micros();
  float dt = (now - lastUpdateUs) / 1e6f;
  lastUpdateUs = now;
  // A stalled I2C bus or a debugger breakpoint could otherwise hand the
  // filter an enormous dt, which a single gyro integration step would turn
  // into a wild, wrong angle. Clamp to something a 200 Hz loop should never
  // legitimately exceed.
  if (dt <= 0 || dt > 0.05f) dt = 1.0f / 200.0f;

  if (!rd(REG_ACCEL_XOUT_H, b, 14)) {
    s.fault = true;
    s.roll_deg = rollF - zeroRoll;
    s.pitch_deg = pitchF - zeroPitch;
    s.temp_c = tempC;      // last known good, so the stat frame stays readable
    return s;
  }

  int16_t rax = (int16_t)(b[0] << 8 | b[1]);
  int16_t ray = (int16_t)(b[2] << 8 | b[3]);
  int16_t raz = (int16_t)(b[4] << 8 | b[5]);
  int16_t rt  = (int16_t)(b[6] << 8 | b[7]);
  int16_t rgx = (int16_t)(b[8] << 8 | b[9]);
  int16_t rgy = (int16_t)(b[10] << 8 | b[11]);
  int16_t rgz = (int16_t)(b[12] << 8 | b[13]);

  float ax = rax / ACCEL_LSB_PER_G - offAx;
  float ay = ray / ACCEL_LSB_PER_G - offAy;
  float az = raz / ACCEL_LSB_PER_G - offAz;
  float gx = rgx / GYRO_LSB_PER_DPS - offGx;
  float gy = rgy / GYRO_LSB_PER_DPS - offGy;
  float gz = rgz / GYRO_LSB_PER_DPS - offGz;

  // Datasheet conversion. Reported rather than compensated for: a per-part
  // temperature coefficient would have to be measured per sensor, whereas the
  // continuous bias tracking below removes the drift without needing to know
  // where it came from. The number is here so it is possible to tell that
  // story apart from a network problem when both look like "it lags outside".
  tempC = rt / 340.0f + 36.53f;
  if (calibrated && fabsf(tempC - calibTempC) > TEMP_DRIFT_WARN_C &&
      millis() - lastTempWarnMs > TEMP_WARN_INTERVAL_MS) {
    lastTempWarnMs = millis();
    Serial.printf("!! IMU at %.1f C, %.1f C from calibration (%.1f C) — re-zero if aim has walked\n",
                  tempC, tempC - calibTempC, calibTempC);
  }

  // Accelerometer-only estimate: correct at rest, meaningless mid-swing once
  // linear acceleration swamps gravity. This is exactly what the reference
  // project used as its ONLY estimate — fine standing still, useless swinging.
  float rollAcc  = atan2f(ay, az) * 180.0f / PI;
  float pitchAcc = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;

  if (firstSample) {
    // Seed the filter from the accelerometer so it starts near the truth
    // instead of climbing there from zero over the first second.
    rollF = rollAcc;
    pitchF = pitchAcc;
    firstSample = false;
  } else {
    // Complementary filter: integrate the gyro (correct instantly, including
    // mid-swing, but drifts) and let the accelerometer pull it back toward
    // gravity-truth slowly (correct only at rest, but has no drift). Neither
    // sensor alone is adequate for a swing; together they cover for each
    // other's blind spot.
    rollF  = FUSION_ALPHA * (rollF  + gx * dt) + (1 - FUSION_ALPHA) * rollAcc;
    pitchF = FUSION_ALPHA * (pitchF + gy * dt) + (1 - FUSION_ALPHA) * pitchAcc;
  }

  // ---- continuous drift correction while the paddle is still ----
  // See config.h for why this exists: without it the fused angle slowly leans
  // to one side as the gyro's zero-rate output wanders, and the paddle ends up
  // parked off-centre so one side of the table becomes hard to reach.
  const float aMag = sqrtf(ax * ax + ay * ay + az * az);

  // A rest test the gyro gets no vote in. Gravity's measured direction only
  // moves if the paddle actually rotates, so an accel-derived angle that has
  // not budged from its own slow average means "not rotating" — a statement
  // that stays true no matter how far the gyro's zero has wandered.
  if (!accAvgSeeded) {
    accRollFast = accRollSlow = rollAcc;
    accPitchFast = accPitchSlow = pitchAcc;
    accAvgSeeded = true;
  }
  accRollFast  += (rollAcc  - accRollFast)  * ACC_REST_FAST;
  accPitchFast += (pitchAcc - accPitchFast) * ACC_REST_FAST;
  accRollSlow  += (rollAcc  - accRollSlow)  * ACC_REST_TRACK;
  accPitchSlow += (pitchAcc - accPitchSlow) * ACC_REST_TRACK;
  const bool accAtRest = fabsf(aMag - 1.0f) < STILL_ACC_TOL &&
                         fabsf(accRollFast  - accRollSlow)  < ACC_REST_TOL_DEG &&
                         fabsf(accPitchFast - accPitchSlow) < ACC_REST_TOL_DEG;

  const bool still = accAtRest &&
                     fabsf(gx) < STILL_GYRO_DPS &&
                     fabsf(gy) < STILL_GYRO_DPS &&
                     fabsf(gz) < STILL_GYRO_DPS;

  if (still) {
    // gx/gy/gz are already bias-corrected, so nudging the stored offset by a
    // fraction of what remains drives the corrected rate toward zero.
    offGx += gx * GYRO_BIAS_TRACK;
    offGy += gy * GYRO_BIAS_TRACK;
    offGz += gz * GYRO_BIAS_TRACK;

    if (stillSinceMs == 0) stillSinceMs = millis();
    if (millis() - stillSinceMs > STILL_MS_BEFORE_RECENTRE) {
      zeroRoll  += (rollF  - zeroRoll)  * AUTO_ZERO_RATE;
      zeroPitch += (pitchF - zeroPitch) * AUTO_ZERO_RATE;
    }
  } else {
    if (accAtRest) {
      // Gravity says stationary, the gyro says otherwise: the gyro is wrong,
      // and the amount it is wrong by is exactly the bias to remove. This is
      // the branch that recovers from a thermal shift large enough to have
      // locked the branch above out of ever running again (see config.h).
      offGx += gx * GYRO_BIAS_RECOVER;
      offGy += gy * GYRO_BIAS_RECOVER;
      offGz += gz * GYRO_BIAS_RECOVER;
    }
    stillSinceMs = 0;
  }

  s.roll_deg  = rollF  - zeroRoll;
  s.pitch_deg = pitchF - zeroPitch;
  s.wx_dps = gx; s.wy_dps = gy; s.wz_dps = gz;
  s.temp_c = tempC;
  s.fault = false;
  return s;
}

float imuTempC() { return tempC; }
float imuCalibTempC() { return calibTempC; }

void imuZero() {
  zeroRoll = rollF;
  zeroPitch = pitchF;
  calibrated = true;
  stillSinceMs = 0;   // a manual re-zero restarts the auto-recentre timer
  Serial.println("-- re-zeroed --");
}

bool imuIsCalibrated() { return calibrated; }
