# Wire Protocol

WebSocket, binary frames, little-endian. Path `/ws`, port 80. No JSON: at
100 Hz, a JSON object per frame means allocating and serializing a string on a
device with no garbage collector, for a payload that is two numbers. A fixed
20-byte binary frame is a `memcpy` and nothing else.

## Server → client: telemetry (20 bytes, sent at up to 100 Hz)

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 1 | magic | always `0xA7` — a client that sees anything else knows the stream is out of sync or it's connected to the wrong thing |
| 1 | 1 | flags | bit 0 `swingActive` (Phase 3) &middot; bit 1 `calibrated` (re-zero has happened at least once) &middot; bit 2 `imuFault` (last I2C read failed — orientation is stale) &middot; bit 3 `button` (unused) |
| 2 | 2 | seq | `u16`, wraps. Lets a client notice a dropped frame. |
| 4 | 4 | t_ms | `u32`, ESP32 `millis()` when this sample was taken |
| 8 | 2 | roll | `i16`, centi-degrees (divide by 100) |
| 10 | 2 | pitch | `i16`, centi-degrees |
| 12 | 2 | wx | `i16`, deg/s &times;10 |
| 14 | 2 | wy | `i16`, deg/s &times;10 |
| 16 | 2 | wz | `i16`, deg/s &times;10 |
| 18 | 2 | swingPeak | `i16`, deg/s &times;10. The running peak gyro magnitude of the *current* swing, updated every frame while bit 0 of flags is set; 0 whenever it is not. Deliberately not a one-shot "swing complete" event — see below. |

`wx/wy/wz` ride along specifically so the client can dead-reckon the paddle
forward by the measured round-trip latency instead of rendering whatever
orientation happened to arrive last (PLAN.md section 4).

**Why swingPeak is continuous, not an event:** waiting for a swing to finish
before reporting it means reporting it after the moment the ball needed
hitting. The server flips `swingActive` the instant gyro magnitude crosses a
rise threshold and reports the peak-so-far every frame from then on
(`swing.cpp`, hysteresis rise/fall thresholds in `config.h`). The client reads
whatever that value is at the exact frame the ball reaches the paddle, instead
of waiting on a discrete "swing complete" message that would already be late.

Roll and pitch are gravity-referenced and re-zeroed on request (`OP_REZERO`);
they are stable indefinitely. Yaw is not transmitted — with no magnetometer to
anchor it, integrating gyro-Z alone drifts without bound, so it was left out
rather than shipped as a number that quietly rots.

## Client → server: control (1–5 bytes, single-byte opcode first)

| opcode | payload | effect |
|---|---|---|
| `0x01` PING | 4 bytes: client's own `u32` timestamp (any units, any epoch) | Server replies with `0x01` + the same 4 bytes echoed + its own `u32 millis()` (9 bytes total). The client never needs clock sync with the ESP32 — it just measures its own timestamp before send and after reply and takes the difference for RTT. |
| `0x02` REZERO | none | The next transmitted roll/pitch become `(0, 0)`. Recentering happens on the device so every client — the scope, the game, anything else — gets "tilt from wherever you're holding it now" for free, instead of each reimplementing it. |
| `0x03` SETRATE | 1 byte: desired rate in Hz | Clamped to 10–200 Hz server-side. Changes how often telemetry is sent; sampling stays fixed at 200 Hz regardless. |

## Why sampling and sending are different rates

The IMU is read at a fixed 200 Hz no matter what the WebSocket is doing —
that's what gives the complementary filter (`imu.cpp`) a `dt` it can trust.
Telemetry is sent at up to 100 Hz, decoupled from sampling, so a slow or
congested link degrades to fewer, still-accurate frames rather than corrupting
the filter that produced them.
