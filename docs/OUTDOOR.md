# Playing outdoors

Indoors, in a small room, this works with barely-perceptible lag. Outdoors it
can lag by a visible fraction of a second — the paddle on screen following your
hand rather than tracking it. Same firmware, same phone, same everything.

The reason is not one fault. It is five, and they stack. This is what changes
when you step outside, in the order that it matters.

---

## 1. The radio link gets much worse — and a small room is why it looked good

The intuition is backwards: outdoors *feels* like it should be better, since
there is nothing in the way. In fact a small room is close to the best 2.4 GHz
environment there is. Every wall, ceiling and floor reflects a second copy of
the signal, so even when your own body is between the paddle in your hand and
the phone across the room, a bounced copy still arrives. Outdoors there are no
reflections. The only path left is the direct one, and that path runs straight
through your arm and torso, which costs 10–20 dB at 2.4 GHz. A link that was
sitting at −40 dBm indoors can be at −75 dBm in a field at the same distance.

WiFi handles that by dropping to a slower, more robust modulation and by
retransmitting. Both cost airtime. Neither is visible from the outside.

## 2. Slow link + fixed 100 Hz send rate = a queue, and a queue is the lag

This is the one that produces the specific symptom "it's a second behind, and it
gets worse the longer I play".

Telemetry used to be broadcast every 10 ms unconditionally, whether or not the
previous frame had actually left the device. On a fast link that is fine — the
queue drains faster than it fills. On a slow one, frames accumulate in the TCP
send queue. And because WebSocket runs over TCP, the phone **cannot skip to the
newest orientation**: TCP guarantees order, so it must be handed every stale
frame first. A hundred queued frames is a full second of backlog, and the
backlog grows for as long as you keep generating frames faster than the link
drains them.

That is not a slow link. That is a queue. No amount of signal strength fixes it,
because the sender was the thing overproducing.

**Fixed** in `net.cpp`: a frame is only sent if the previous one has drained
(`availableForWriteAll`). Otherwise it is dropped — the next sample is 10 ms
away and strictly better than the one being discarded. Sustained dropping also
backs the send rate off toward 30 Hz, so fewer packets contend for the airtime
that is left. Latency is now bounded no matter how bad the link gets; what
degrades is the update rate, which is what the client's extrapolation is for.

## 3. Nothing was compensating for latency, though the design said it would

`PLAN.md` section 4 budgeted 40–80 ms end-to-end and specified that angular
velocity would ship alongside orientation so the client could dead-reckon
forward by the measured round trip. The protocol has carried `wx/wy/wz` for
exactly that purpose since Phase 1 — and the game threw them away and rendered
whichever orientation last arrived. Indoors that is a barely-visible ~50 ms.
Outdoors it is most of the complaint.

**Fixed** in `web/game.html`: it now pings for RTT twice a second and projects
roll/pitch forward by `rtt/2 + (age of the newest frame)`, clamped to 120 ms and
±12°, freezing rather than extrapolating once a frame is over 300 ms old.

## 4. The phone throttles itself in the sun, and the smoothing amplified it

Direct sunlight means the screen goes to maximum brightness and the phone
absorbs solar heat on top of that. Phones respond by thermal-throttling the GPU
and dropping the frame rate — 60 fps indoors becomes 30 or 20 fps in a field.

The paddle smoothing constant was applied once per rendered frame, which
silently made it a *lag* control tied to frame rate. Measured: settling to 95%
took 50 ms at 60 fps and **150 ms at 20 fps** — 100 ms of extra lag appearing
for no reason other than the phone being hot, stacked on top of everything the
network was already costing.

**Fixed**: the constant is rebased on elapsed time, so the feel is identical at
20 fps and 60 fps.

## 5. Sunlight on the paddle, on two separate paths

**The sensor.** A MEMS gyro's zero-rate output moves with die temperature. A
paddle in direct sun goes from a ~25 °C room to 50–60 °C within minutes, so the
one-shot calibration taken at boot is describing a sensor that no longer exists.
Worse, there was a latch: the continuous bias tracker that exists to correct
exactly this only ran while the paddle read as "still", and stillness was judged
by the gyro. Once thermal drift pushed the corrected rate past that threshold,
the paddle never read as still again — so the tracker that would have removed
the drift stopped running, permanently, and could not recover on its own.

**Fixed** in `imu.cpp`: rest is now also detected from the accelerometer alone
(gravity's direction does not move unless the paddle actually rotates), which
gives the bias tracker a way back in no matter how far the gyro's zero has
wandered. The die temperature is also read now — it was already being fetched in
the same burst and discarded — and is reported to the game and warned about on
serial when it drifts far from the calibration temperature.

**The screen.** The dark theme is close to unreadable on a phone in direct sun.
A phone cannot out-shine the sun, so the only contrast available outdoors is
between its brightest pixel and its darkest — which means white background, near
black ink, the opposite of what looks good indoors.

**Added**: a **Daylight** toggle (light background, high-contrast, fatter
strokes, black-rimmed ball) and a screen wake lock, since a propped-up phone
outdoors otherwise dims and sleeps mid-rally.

---

## Diagnosing it on the spot

Tap **Stats** in the game. The readout separates faults that feel identical:

```
rtt 42ms   age 12ms   rx 98Hz        <- link round trip, frame age, receive rate
tx 100Hz   drops 0    gaps 0         <- what the paddle sent, and what it dropped
rssi -48dBm  clients 1  imu 31°C     <- signal, connected clients, sensor temp
fps 60                               <- the phone's own rendering
```

| What you see | What it is | What to do |
|---|---|---|
| `rssi` below −75 | Weak link — distance, or your body between paddle and phone | Move the phone to your side of the table, not the far end. Keep it off the ground. |
| `drops` climbing, `tx` under 100 | Link cannot carry the rate; backoff is working as intended | Nothing is broken. If `rtt` is still low the game is fine. |
| `rtt` over 120 ms with `drops` at 0 | Congestion or the phone's WiFi power-saving, not this code | Turn off the phone's battery saver; change `AP_CHANNEL` in `config.h` to 6 or 11. |
| `fps` under 45 with everything else healthy | The phone is thermally throttling | Shade the phone. It is the phone, not the paddle. |
| `clients` above 1 when only the game is open | A previous session's socket has not died yet | It is reaped within a few seconds now; if it persists, reboot the paddle. |
| `imu` more than ~10 °C above where you calibrated | Thermal drift; aim may have walked | Tap **Re-zero**. Keep the paddle out of direct sun between games. |
| `age` large and pinned, `rx` at 0 | Frames have stopped arriving entirely | Check the phone is still on `AirTT-xxxx` and has not silently switched to mobile data. |

## Things this repo cannot fix for you

- **The phone silently leaving the AP.** The paddle's network has no internet.
  Both Android and iOS notice this and may deprioritise it, re-scan periodically,
  or switch to mobile data — each of which shows up as a multi-hundred-millisecond
  stall or a dead link. Outdoors you are usually on mobile data, so this is far
  more likely there than at home. Tell the phone to stay connected when prompted,
  and turn off any "switch to mobile data when WiFi is poor" / adaptive
  connectivity setting.
- **The phone's own WiFi power saving.** The ESP32 disables power save on its
  own radio, but it cannot disable the phone's. A phone in battery saver
  parks its radio between beacons and adds 100 ms+ spikes. Take it off battery
  saver and keep the screen on (the wake lock now helps with this).
- **The powerbank.** Heat makes protection circuits trip earlier, and many banks
  cut out below ~100 mA — which the ESP32 can drop under. A paddle that dies
  mid-rally and reboots looks exactly like a network fault. Check for the boot
  banner on serial if you can, or for the score resetting.
