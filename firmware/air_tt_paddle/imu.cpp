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
  long sax = 0, say = 0, saz = 0, sgx = 0, sgy = 0, sgz = 0;

  for (int i = 0; i < N; i++) {
    uint8_t b[14];
    if (rd(REG_ACCEL_XOUT_H, b, 14)) {
      sax += (int16_t)(b[0] << 8 | b[1]);
      say += (int16_t)(b[2] << 8 | b[3]);
      saz += (int16_t)(b[4] << 8 | b[5]);
      sgx += (int16_t)(b[8] << 8 | b[9]);
      sgy += (int16_t)(b[10] << 8 | b[11]);
      sgz += (int16_t)(b[12] << 8 | b[13]);
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

  Serial.printf("calibration done. accel off (g): %.3f,%.3f,%.3f  gyro bias (dps): %.2f,%.2f,%.2f\n",
                offAx, offAy, offAz, offGx, offGy, offGz);
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
    return s;
  }

  int16_t rax = (int16_t)(b[0] << 8 | b[1]);
  int16_t ray = (int16_t)(b[2] << 8 | b[3]);
  int16_t raz = (int16_t)(b[4] << 8 | b[5]);
  int16_t rgx = (int16_t)(b[8] << 8 | b[9]);
  int16_t rgy = (int16_t)(b[10] << 8 | b[11]);
  int16_t rgz = (int16_t)(b[12] << 8 | b[13]);

  float ax = rax / ACCEL_LSB_PER_G - offAx;
  float ay = ray / ACCEL_LSB_PER_G - offAy;
  float az = raz / ACCEL_LSB_PER_G - offAz;
  float gx = rgx / GYRO_LSB_PER_DPS - offGx;
  float gy = rgy / GYRO_LSB_PER_DPS - offGy;
  float gz = rgz / GYRO_LSB_PER_DPS - offGz;

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

  s.roll_deg  = rollF  - zeroRoll;
  s.pitch_deg = pitchF - zeroPitch;
  s.wx_dps = gx; s.wy_dps = gy; s.wz_dps = gz;
  s.fault = false;
  return s;
}

void imuZero() {
  zeroRoll = rollF;
  zeroPitch = pitchF;
  calibrated = true;
  Serial.println("-- re-zeroed --");
}

bool imuIsCalibrated() { return calibrated; }
