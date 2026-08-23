// Swing detection: a hysteresis state machine on gyro magnitude.
//
// Deliberately NOT "wait for the swing to finish, then report its peak" — by
// the time a swing is over, the ball needed hitting 50-100ms ago. Instead
// this flips active the instant the rise threshold is crossed and reports
// the running peak-so-far on every frame while active. The client (game.html)
// samples "is a swing happening, how hard so far" at the exact moment the
// ball reaches the paddle, rather than waiting on a discrete "swing complete"
// event that would arrive too late to matter (PLAN.md section 4 latency budget).
#pragma once
#include <stdint.h>

// Called once per IMU sample (200 Hz) with the current gyro magnitude
// (deg/s, sqrt(wx^2+wy^2+wz^2)) and the sample time.
void swingUpdate(float gyroMagDps, uint32_t nowMs);

bool  swingIsActive();
float swingPeakDps();   // running peak this swing; 0 when not active
