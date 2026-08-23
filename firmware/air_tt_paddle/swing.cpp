#include "swing.h"
#include "config.h"

static bool active = false;
static float peak = 0;

void swingUpdate(float gyroMagDps, uint32_t nowMs) {
  (void)nowMs;
  if (!active) {
    if (gyroMagDps > SWING_RISE_DPS) { active = true; peak = gyroMagDps; }
  } else {
    if (gyroMagDps > peak) peak = gyroMagDps;
    // Hysteresis, not a single threshold: without a gap between the rise and
    // fall points, noise sitting right at the boundary flickers active/idle
    // many times a second. The gap is what makes one swing read as one swing.
    if (gyroMagDps < SWING_FALL_DPS) { active = false; peak = 0; }
  }
}

bool  swingIsActive() { return active; }
float swingPeakDps()  { return active ? peak : 0; }
