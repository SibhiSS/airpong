/*
 * Air Table Tennis — Phase 0: bring-up and sanity check.
 *
 * This sketch is NOT the game. Its only job is to answer four questions before
 * a single line of game code gets written:
 *
 *   1. What board is this really? (chip, flash, MAC)
 *   2. Is the MPU-6050 wired correctly and talking? (I2C scan + WHO_AM_I)
 *   3. Do the sensor ranges survive a real swing, or do they clip?
 *   4. Does the powerbank keep the ESP32 alive, or cut out when idle?
 *
 * Question 3 is the important one. The reference project runs the MPU at its
 * default +/-2 g and +/-250 dps, which a genuine wrist swing blows straight
 * through — the sensor pins at its rail and the data is a flat line exactly
 * during the moment the game cares about. We configure +/-8 g and +/-2000 dps
 * and then explicitly count clipped samples so we can prove it.
 *
 * Serial: 115200 baud.
 *   Streams CSV at 200 Hz for tools/scope.py, and prints a summary every 2 s.
 *   Send 'r' to reset the peak/clip counters, 's' to toggle the CSV stream.
 */

#include <Wire.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <WiFi.h>

// ---------------- pins & I2C ----------------
// Do NOT hardcode GPIO numbers here. Every ESP32 board definition declares its
// own default I2C pins as SDA/SCL, and boards that silkscreen their headers as
// D1/D2/D3 rather than as raw GPIO numbers still map those labels onto the same
// constants. Using them means this sketch is correct on any board, and the
// setup banner prints which physical GPIOs they landed on so we can confirm
// against the silkscreen instead of guessing.
//
// To override (e.g. the sensor is wired somewhere non-default), set these:
#ifndef PIN_SDA_OVERRIDE
  #define PIN_SDA_OVERRIDE  -1
  #define PIN_SCL_OVERRIDE  -1
#endif

static const int PIN_SDA = (PIN_SDA_OVERRIDE >= 0) ? PIN_SDA_OVERRIDE : SDA;
static const int PIN_SCL = (PIN_SCL_OVERRIDE >= 0) ? PIN_SCL_OVERRIDE : SCL;
static const uint32_t I2C_HZ = 400000;

// ---------------- MPU-6050 registers ----------------
static const uint8_t REG_SMPLRT_DIV   = 0x19;
static const uint8_t REG_CONFIG       = 0x1A;
static const uint8_t REG_GYRO_CONFIG  = 0x1B;
static const uint8_t REG_ACCEL_CONFIG = 0x1C;
static const uint8_t REG_ACCEL_XOUT_H = 0x3B;
static const uint8_t REG_PWR_MGMT_1   = 0x6B;
static const uint8_t REG_WHO_AM_I     = 0x75;

// Chosen ranges. See the header comment for why these are not the defaults.
static const uint8_t AFS_SEL = 2;   // 2 -> +/-8 g,     4096 LSB/g
static const uint8_t FS_SEL  = 3;   // 3 -> +/-2000 dps, 16.4 LSB/(deg/s)
static const uint8_t DLPF    = 3;   // 3 -> ~44 Hz accel / 42 Hz gyro, 1 kHz base
static const uint8_t SMPLRT  = 4;   // 1000/(1+4) = 200 Hz

static const float ACCEL_LSB_PER_G   = 4096.0f;
static const float GYRO_LSB_PER_DPS  = 16.4f;

// A raw reading at +/-32767 means the ADC is pinned: the true value is
// somewhere off the top of the scale and we have no idea where.
static const int16_t CLIP_LIMIT = 32000;

static uint8_t  mpuAddr = 0x68;
static bool     mpuOk   = false;
static bool     streaming = true;

// peak / clip tracking
static float    peakG = 0.0f, peakDps = 0.0f;
static uint32_t clipAccel = 0, clipGyro = 0, sampleCount = 0;
static uint32_t lastSummary = 0;
static uint32_t nextSampleUs = 0;
static const uint32_t SAMPLE_INTERVAL_US = 5000;  // 200 Hz

// ---------------- low-level I2C helpers ----------------
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

// ---------------- 1. what board is this ----------------
static void reportBoard() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);

  const char *model = "unknown";
  switch (chip.model) {
    case CHIP_ESP32:   model = "ESP32";    break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    default: break;
  }

  uint32_t flashBytes = 0;
  esp_flash_get_size(NULL, &flashBytes);

  Serial.println();
  Serial.println("=========== BOARD ===========");
  Serial.printf("chip        : %s rev %d, %d core(s)\n", model, chip.revision, chip.cores);
  Serial.printf("features    : %s%s%s\n",
                (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi-bgn " : "",
                (chip.features & CHIP_FEATURE_BT)       ? "BT "       : "",
                (chip.features & CHIP_FEATURE_BLE)      ? "BLE"       : "");
  Serial.printf("flash       : %u bytes (%u MB)\n", flashBytes, flashBytes / (1024 * 1024));
  Serial.printf("free heap   : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("MAC (STA)   : %s\n", WiFi.macAddress().c_str());
  Serial.printf("SDK         : %s\n", ESP.getSdkVersion());
  Serial.printf("I2C pins    : SDA=GPIO%d  SCL=GPIO%d   <- wire the MPU to THESE\n",
                PIN_SDA, PIN_SCL);
  Serial.println("=============================");
  Serial.println("If your board's headers are labelled D1/D2/D3 instead of GPIO");
  Serial.println("numbers, find the two pins whose GPIO numbers match the line");
  Serial.println("above. On most D-labelled ESP32 boards that is D2=SDA, D1=SCL.");

  // The plan assumes 4 MB and the `default` partition scheme, which leaves
  // ~1.5 MB of SPIFFS/LittleFS for the gzipped three.js game. Flag a mismatch
  // now rather than when the filesystem image refuses to fit.
  if (flashBytes < 4 * 1024 * 1024) {
    Serial.println("!! WARNING: under 4 MB of flash. The web build may not fit.");
    Serial.println("!! Revisit the partition scheme in PLAN.md section 10.");
  }
}

// ---------------- 2. is the MPU wired up ----------------
static void i2cScan() {
  Serial.println("\n--- I2C scan ---");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", a,
                    (a == 0x68 || a == 0x69) ? "   <- MPU-6050" : "");
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  NOTHING FOUND.");
    Serial.println("  Check: VCC->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22.");
    Serial.println("  A dead-silent bus is almost always power or a swapped SDA/SCL.");
  }
}

static bool mpuInit() {
  uint8_t who = 0;

  // AD0 low gives 0x68, AD0 high or floating gives 0x69. Try both.
  for (uint8_t addr : {(uint8_t)0x68, (uint8_t)0x69}) {
    mpuAddr = addr;
    if (rd(REG_WHO_AM_I, &who, 1)) {
      Serial.printf("\nMPU at 0x%02X, WHO_AM_I = 0x%02X\n", addr, who);
      break;
    }
    who = 0;
  }
  if (who == 0) { Serial.println("\nMPU-6050 not responding."); return false; }

  // WHO_AM_I is 0x68 on a real MPU-6050. Clones (MPU-6500, MPU-9250) report
  // something else but are usually register-compatible enough for this, so
  // warn rather than refuse.
  if (who != 0x68) {
    Serial.printf("note: WHO_AM_I is 0x%02X, not 0x68 — likely a clone. Continuing.\n", who);
  }

  // PLL with X-axis gyro reference is more stable than the internal oscillator.
  if (!wr(REG_PWR_MGMT_1, 0x01)) return false;
  delay(50);
  if (!wr(REG_CONFIG, DLPF)) return false;
  if (!wr(REG_SMPLRT_DIV, SMPLRT)) return false;
  if (!wr(REG_GYRO_CONFIG,  (uint8_t)(FS_SEL  << 3))) return false;
  if (!wr(REG_ACCEL_CONFIG, (uint8_t)(AFS_SEL << 3))) return false;
  delay(50);

  // Read the config back. Writing a register and assuming it took is how you
  // spend an afternoon debugging a filter that was never the problem.
  // SMPLRT_DIV(0x19), CONFIG(0x1A), GYRO_CONFIG(0x1B), ACCEL_CONFIG(0x1C) are
  // contiguous, so one burst read gets all four.
  uint8_t v[4] = {0, 0, 0, 0};
  if (!rd(REG_SMPLRT_DIV, v, 4)) { Serial.println("config read-back failed"); return false; }
  const uint8_t srd = v[0], cfg = v[1], gc = v[2], ac = v[3];

  Serial.println("--- MPU configuration (read back) ---");
  Serial.printf("  DLPF_CFG    : %d  (~44 Hz)\n", cfg & 0x07);
  Serial.printf("  SMPLRT_DIV  : %d  -> %d Hz\n", srd, 1000 / (1 + srd));
  Serial.printf("  FS_SEL      : %d  -> +/-%d dps\n", (gc >> 3) & 3, 250 << ((gc >> 3) & 3));
  Serial.printf("  AFS_SEL     : %d  -> +/-%d g\n",  (ac >> 3) & 3, 2 << ((ac >> 3) & 3));

  bool match = ((cfg & 7) == DLPF) && (srd == SMPLRT) &&
               (((gc >> 3) & 3) == FS_SEL) && (((ac >> 3) & 3) == AFS_SEL);
  Serial.println(match ? "  config verified OK" : "  !! CONFIG MISMATCH — writes did not stick");
  return match;
}

// ---------------- 3. does a real swing clip ----------------
static void sampleOnce() {
  uint8_t b[14];
  if (!rd(REG_ACCEL_XOUT_H, b, 14)) return;

  int16_t ax = (int16_t)(b[0]  << 8 | b[1]);
  int16_t ay = (int16_t)(b[2]  << 8 | b[3]);
  int16_t az = (int16_t)(b[4]  << 8 | b[5]);
  int16_t tR = (int16_t)(b[6]  << 8 | b[7]);
  int16_t gx = (int16_t)(b[8]  << 8 | b[9]);
  int16_t gy = (int16_t)(b[10] << 8 | b[11]);
  int16_t gz = (int16_t)(b[12] << 8 | b[13]);

  if (abs(ax) > CLIP_LIMIT || abs(ay) > CLIP_LIMIT || abs(az) > CLIP_LIMIT) clipAccel++;
  if (abs(gx) > CLIP_LIMIT || abs(gy) > CLIP_LIMIT || abs(gz) > CLIP_LIMIT) clipGyro++;

  float axg = ax / ACCEL_LSB_PER_G,  ayg = ay / ACCEL_LSB_PER_G,  azg = az / ACCEL_LSB_PER_G;
  float gxd = gx / GYRO_LSB_PER_DPS, gyd = gy / GYRO_LSB_PER_DPS, gzd = gz / GYRO_LSB_PER_DPS;
  float tempC = tR / 340.0f + 36.53f;

  float aMag = sqrtf(axg * axg + ayg * ayg + azg * azg);
  float gMag = sqrtf(gxd * gxd + gyd * gyd + gzd * gzd);
  if (aMag > peakG)   peakG   = aMag;
  if (gMag > peakDps) peakDps = gMag;
  sampleCount++;

  // CSV for tools/scope.py: t_ms,ax,ay,az,gx,gy,gz,aMag,gMag,tempC
  if (streaming) {
    Serial.printf("D,%lu,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.3f,%.1f,%.1f\n",
                  millis(), axg, ayg, azg, gxd, gyd, gzd, aMag, gMag, tempC);
  }
}

static void summary() {
  Serial.printf("S,uptime=%lus  samples=%lu  peak=%.2fg/%.0fdps  clip: accel=%lu gyro=%lu  heap=%u\n",
                millis() / 1000, sampleCount, peakG, peakDps, clipAccel, clipGyro, ESP.getFreeHeap());

  if (clipAccel || clipGyro) {
    Serial.println("!! CLIPPING DETECTED — the sensor is pinned at its rail during swings.");
    Serial.println("!! Raise AFS_SEL/FS_SEL. Swing data is unusable while this is nonzero.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n### Air TT — Phase 0 bring-up ###");

  reportBoard();

  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  delay(100);
  i2cScan();
  mpuOk = mpuInit();

  if (mpuOk) {
    Serial.println("\nStreaming at 200 Hz. Commands: 'r' reset peaks, 's' toggle stream.");
    Serial.println("\n>>> NOW: hold it still 3 s, then swing as hard as you will in a game.");
    Serial.println(">>> Phase 0 passes when peak g and dps look real AND clip counts stay 0.\n");
  } else {
    Serial.println("\nMPU init failed — fix the wiring before going further.");
  }
  lastSummary  = millis();
  nextSampleUs = micros();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r') {
      peakG = peakDps = 0; clipAccel = clipGyro = sampleCount = 0;
      Serial.println("-- peaks and clip counters reset --");
    } else if (c == 's') {
      streaming = !streaming;
      Serial.printf("-- stream %s --\n", streaming ? "ON" : "OFF");
    }
  }

  if (!mpuOk) { delay(1000); return; }

  // Fixed-cadence sampling off micros(), not delay(). The fusion filter in
  // Phase 1 needs a dt it can trust, so the habit starts here.
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) >= 0) {
    nextSampleUs += SAMPLE_INTERVAL_US;
    // If we fell badly behind (serial backpressure), resync rather than
    // spin trying to catch up on a backlog of stale slots.
    if ((int32_t)(micros() - nextSampleUs) > (int32_t)(SAMPLE_INTERVAL_US * 4)) {
      nextSampleUs = micros() + SAMPLE_INTERVAL_US;
    }
    sampleOnce();
  }

  if (millis() - lastSummary >= 2000) {
    lastSummary = millis();
    summary();
  }
}
