# Air Table Tennis — Curation & Build Plan

Motion-controlled table tennis. ESP32 + MPU-6050 strapped to your hand is the paddle;
an Android tablet is the screen. Powerbank on the ESP32, no PC in the loop at play time.

---

## 1. Reference audit — `Karthikeyan-code-byte/virtual-tennis-game`

Pulled and read in full. It is ~1000 lines total: one `.ino`, one self-contained
three.js HTML page. It works, and the skeleton (ESP32 → WebSocket → browser canvas)
is the right skeleton. But it is a tilt demo, not an air-TT game.

### What it does right (keep)
- ESP32 hosts an HTTP page **and** a WebSocket server; the browser is the whole client.
- One-click "set center point" software calibration from any wrist resting position.
- Sensitivity / axis-swap / invert exposed in the UI instead of hardcoded.
- Manual keyboard fallback so the game is playable with the hardware unplugged.

### What is broken or limiting (fix)

| # | Issue | Why it matters | Fix |
|---|---|---|---|
| 1 | **Accelerometer-only orientation.** `atan2(accY, accZ)` assumes the only acceleration is gravity. | The instant you actually *swing*, linear acceleration swamps gravity and the paddle flies to a corner. Fatal for a swing-based game. | Fuse gyro + accel (complementary / Mahony filter). Gyro carries fast motion, accel only corrects slow drift. |
| 2 | **Default ±2 g / ±250 dps ranges.** | A real wrist swing hits 4–8 g and 1000+ dps at the sensor. Both clip flat during exactly the moment that matters. | ±8 g accel, ±2000 dps gyro, DLPF ~44 Hz. |
| 3 | **No swing detection at all.** You hit by *holding* the paddle where the ball will be. | That is Pong with a tilt sensor. It is not air table tennis. | Swing state machine on gyro magnitude + forward accel, with a contact timing window. |
| 4 | **three.js loaded from cdnjs.** | If the tablet joins the ESP32's own hotspot there is no internet, and the game is a black screen. | Everything self-hosted from ESP32 flash. Zero external requests. |
| 5 | **STA-only, hardcoded SSID, blocking `while(WiFi.status()...)`.** `WiFi.begin()` is also called twice. | Needs a router or phone hotspot present, and a reflash to change venue. Boot blocks forever if the AP is absent. | Dual mode: try saved STA creds for ~8 s, else fall back to its own SoftAP. |
| 6 | **JSON + `String` built and serialized every 16 ms.** | Heap churn on a device with no GC. ~60 bytes to carry two floats. | Fixed-size little-endian binary frame, 20 bytes, zero allocation. |
| 7 | **Sample rate == send rate == 60 Hz, inside `loop()`.** | Filter `dt` jitters with whatever else `loop()` did. WiFi stalls corrupt the fusion. | Sample at a fixed 200 Hz off a `micros()` accumulator; send at 100 Hz, decoupled. |
| 8 | **No WiFi power-save disable.** | ESP32 default modem sleep injects 100 ms+ latency spikes. Feels like lag, gets blamed on the game. | `WiFi.setSleep(false)` / `esp_wifi_set_ps(WIFI_PS_NONE)`, plus `TCP_NODELAY`. |
| 9 | **No latency compensation.** The client renders whatever arrived, whenever. | End-to-end is 40–80 ms. In a timing game that is the difference between a hit and a miss. | Ship angular velocity alongside orientation; client dead-reckons forward by measured RTT. Seq numbers + RTT ping. |
| 10 | Desktop-shaped UI. | The tablet needs landscape lock, fullscreen, fat touch targets, wake lock. | Purpose-built tablet layout. |

**Verdict: build from scratch, keep the architecture.** The reference is mostly
scene-setup boilerplate — little to salvage line-by-line — but its overall shape
(ESP32 self-hosts the client, WS telemetry, in-page calibration) is correct.

---

## 2. Target architecture

```
   ┌──────────────────────────┐             ┌────────────────────────────┐
   │  ESP32 + MPU-6050        │             │   Android tablet           │
   │  (on your hand)          │             │   (propped up as screen)   │
   │                          │             │                            │
   │  200 Hz  I2C sample      │             │   Chrome, fullscreen       │
   │     ↓                    │   WiFi      │      ↑                     │
   │  complementary filter    │   SoftAP    │   game loop  60 fps        │
   │     ↓                    │  ════════▶  │      ↑                     │
   │  swing state machine     │   WS :80    │   input.js  extrapolate    │
   │     ↓                    │   20 B      │      ↑                     │
   │  binary frame @ 100 Hz   │   @ 100 Hz  │   WebSocket                │
   │                          │             │                            │
   │  HTTP: serves the game   │  ─────────▶ │   loads / from ESP32       │
   │  from LittleFS (gzipped) │  first hit  │   flash. No internet.      │
   └──────────────────────────┘             └────────────────────────────┘
        powerbank via USB
```

**Network.** ESP32 runs `WIFI_AP_STA`. On boot it tries stored STA credentials for
8 s; regardless of the outcome it always raises its own SoftAP `AirTT-xxxx`. The
tablet joins the SoftAP and opens `http://192.168.4.1` — works in a field with no
router. If STA also connected, the same page is reachable on the LAN and the tablet
keeps its internet.

**Why WebSocket and not UDP.** Browsers cannot open UDP sockets. WS-over-TCP with
`TCP_NODELAY` and 20-byte frames on a single-hop SoftAP runs ~2–8 ms typical. The
occasional retransmit spike is what the client-side extrapolation absorbs.

---

## 3. Control scheme — the actual game design

This is the part that decides whether it feels like table tennis or like a tech demo.

**Two inputs, deliberately separated:**

**1. Orientation → aim.** Roll and pitch only, both gravity-referenced and therefore
drift-free. Yaw is deliberately unused — with no magnetometer it drifts without bound.
- roll → paddle X across the table
- pitch → paddle height Y
- the paddle *face normal* also comes from orientation, and decides where the ball goes

**2. Swing → contact.** A state machine on gyro magnitude ‖ω‖ and forward linear
acceleration: `IDLE → WINDUP → STRIKE(peak) → FOLLOW_THROUGH → refractory`.
- **Peak ‖ω‖ → ball speed.** A gentle flick is a soft push; a full swing is a smash.
- **Swing axis vs. face angle at contact → spin.** Closing the face while swinging up
  gives topspin (Magnus dips it onto the table). Open face, downward swipe gives
  backspin/chop — it floats, then dies. A lateral component gives sidespin.
- **Timing window.** The strike must land within roughly ±90 ms of the ball entering
  the strike zone. Early or late gives an edge, a mishit, or a whiff.
- Fired on **threshold crossing, not on peak** — buys back ~30 ms of latency.

**3. Assist levels**, one toggle, three settings, so it is actually playable:
- *Arcade* — the paddle auto-tracks toward the ball in X; you only supply the swing.
- *Normal* — tilt aims, swing hits, generous strike zone. **Default.**
- *Raw* — no assist, real timing window. Hard.

**Ball physics.** Gravity, table bounce with restitution plus a spin-dependent friction
kick, net collision, Magnus force from the spin vector, edge and net-cord luck.
Standard 11-point scoring with 2-serve alternation and deuce.

**Opponent.** AI with a reaction-delay and aim-error budget per difficulty — it should
miss the way a human misses, not the way a broken bot misses. A second player can come
later via touch on the tablet or a second ESP32.

---

## 4. Latency budget (designed against, not hoped for)

| Stage | Budget |
|---|---|
| MPU sample interval (200 Hz) | 5 ms |
| ESP32 fusion + framing | < 1 ms |
| Send interval (100 Hz) | 10 ms |
| WiFi SoftAP single hop | 2–8 ms (spikes to 40) |
| Browser WS → rAF | 0–17 ms |
| Tablet display pipeline | 16–50 ms |
| **Total** | **~40–80 ms** |

Mitigations baked into the design rather than bolted on afterwards:
- angular velocity ships with orientation, so the client dead-reckons forward by RTT/2
- RTT is measured continuously via a ping opcode; the extrapolation horizon tracks it live
- the swing fires on threshold crossing rather than on peak
- the strike-zone window widens by the measured latency, so it self-tunes
- `WiFi.setSleep(false)`, `TCP_NODELAY`, and no `delay()` anywhere in `loop()`

---

## 5. Wiring

| MPU-6050 (GY-521) | ESP32 DevKit | Note |
|---|---|---|
| VCC | 3V3 | 3.3 V preferred over 5 V — keeps I2C at 3.3 V logic |
| GND | GND | |
| SDA | GPIO 21 | I2C bus at 400 kHz |
| SCL | GPIO 22 | |
| AD0 | GND | address `0x68` (floating or high gives `0x69`) |
| INT | GPIO 19 | optional — data-ready IRQ, jitter-free sampling |
| XDA / XCL | n/c | |

Mount the MPU in a **known, rigid** orientation relative to your hand. The axis
convention is baked into the firmware, and a board that rotates inside its tape is an
unfixable bug. Tape or zip-tie it to something flat — an old paddle blade, a phone
case, a piece of foam board. Keep the wires short.

---

## 6. Repo layout

```
Air_table_tennis/
├─ PLAN.md
├─ firmware/air_tt_paddle/
│   ├─ air_tt_paddle.ino
│   ├─ config.h        pins, ranges, WiFi, tuning constants — one place
│   ├─ imu.h/.cpp      MPU-6050 register-level driver + complementary filter
│   ├─ swing.h/.cpp    swing state machine, spin estimate
│   ├─ net.h/.cpp      AP_STA, async HTTP from LittleFS, WS, binary framing
│   └─ data/           gzipped web build, flashed to LittleFS
├─ web/
│   ├─ index.html      tablet layout, landscape lock, wake lock, fullscreen
│   ├─ input.js        WS client, decode, RTT, extrapolation, calibration
│   ├─ physics.js      ball, spin/Magnus, table, net, scoring
│   ├─ render.js       the renderer
│   ├─ ai.js           opponent
│   └─ style.css
├─ tools/
│   ├─ build_fs.py     minify + gzip web/ → firmware/.../data/
│   ├─ fake_paddle.py  simulated ESP32: speaks the real protocol from synthetic
│   │                  or recorded swings, so the whole game can be built and
│   │                  tested on the PC with no hardware attached
│   └─ scope.py        live plot of roll / pitch / gyro / swing-state while you
│                      swing — this is how the swing thresholds get tuned
└─ docs/
    ├─ WIRING.md
    ├─ PROTOCOL.md
    └─ TUNING.md
```

`fake_paddle.py` and `scope.py` are not nice-to-haves. They are what makes this
buildable without a two-minute flash cycle per iteration.

---

## 7. Wire protocol (draft — locked in during Phase 1)

Little-endian, fixed 20 bytes, binary WS frame. No JSON, no allocation.

```
off  sz  field        units
  0   1  magic        0xA7
  1   1  flags        b0 swingActive  b1 calibrated  b2 imuFault  b3 button
  2   2  seq          u16, wrapping
  4   4  t_ms         u32, ESP32 millis at sample time
  8   2  roll         i16, centi-degrees
 10   2  pitch        i16, centi-degrees
 12   2  wx           i16, dps × 10   ┐
 14   2  wy           i16, dps × 10   ├ for client-side extrapolation
 16   2  wz           i16, dps × 10   ┘
 18   2  swingPeak    i16, dps × 10 — 0 unless a strike fired this frame
```

Client → ESP32 is single-byte opcodes: `0x01` ping (RTT), `0x02` re-zero,
`0x03` set rate.

---

## 8. Phases

**Phase 0 — bring-up.** Blink, I2C scan, dump raw MPU registers over serial, confirm
the ranges and that nothing clips when you swing hard. Confirm the powerbank does not
cut out. *Gate: raw data looks sane on a scope plot.*

**Phase 1 — telemetry pipeline.** Fusion filter, binary protocol, AP_STA, WS server,
`scope.py`. *Gate: rock-steady roll/pitch on the PC plot, including during a hard
swing, under 15 ms.*

**Phase 2 — game engine, no hardware.** The full game against `fake_paddle.py` in the
browser on the PC: physics, spin, AI, scoring, renderer. *Gate: fun with a mouse.*

**Phase 3 — swing feel.** Swing state machine on-device, thresholds tuned with
`scope.py` against your actual swings. Latency compensation live. *Gate: a smash
feels like a smash and a chop floats.*

**Phase 4 — tablet.** LittleFS hosting, gzip, landscape / fullscreen / wake-lock,
touch UI, audio, on-screen calibration. *Gate: powerbank and tablet only, no PC,
full match.*

**Phase 5 — polish.** Sound, particles, replays, difficulty, localStorage leaderboard,
README with photos.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| **Powerbank auto-shutoff.** ESP32 idles at ~80–150 mA; many banks cut off below ~100 mA and will kill you mid-rally. | Test in Phase 0. If it cuts out: a bank with a low-current / trickle mode, or a 1000 mAh LiPo + TP4056, or a bleed resistor. |
| **Swinging a powerbank is horrible.** | Only the ESP32 + MPU go on the hand, with a short USB cable to the bank in a pocket or on an armband. Decide the strap in Phase 0, not Phase 4. |
| WiFi latency spikes in a crowded 2.4 GHz room. | SoftAP on a scanned-quiet channel, power-save off; extrapolation absorbs the rest. |
| Tablet browser throttles background or idle tabs. | Wake lock, fullscreen, keep-alive — and test on the actual tablet in Phase 2, not Phase 4. |
| Gyro bias drift over a long session. | Auto re-zero whenever the paddle is detected still for > 1.5 s, plus a manual re-zero button. |
| Swing thresholds are personal. | They live in `config.h` and are also settable live from the UI; `scope.py` makes tuning a 30-second job. |

---

## 10. Decisions (settled 2026-08-23)

1. **Renderer — self-hosted three.js.** Real 3D, gzipped into LittleFS, zero external
   requests. Consequences: the LittleFS partition must hold ~150 KB of library plus the
   game, so the board needs a partition scheme with a decent SPIFFS/LittleFS region;
   and WebGL capability on the tablet becomes a **Phase 2 gate**, not a Phase 4
   surprise — the very first thing rendered gets opened on the actual tablet.
2. **Toolchain — `arduino-cli`.** Compile, flash, and serial-monitor driven from this
   machine, so firmware can be debugged directly rather than handed over as sketches.
3. **Board — ESP32-WROOM-32 DevKit.** 4 MB flash, GPIO 21/22 for I2C, FQBN
   `esp32:esp32:esp32`. Partition scheme: `default` (1.2 MB app / 1.5 MB SPIFFS) —
   ample for the gzipped web build.
