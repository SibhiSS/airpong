# Air TT

Motion-controlled table tennis. An ESP32 with an MPU-6050 taped to a paddle
(or your hand) is the controller; a phone or tablet browser is the screen.
No app to install — the ESP32 hosts the game itself and you just connect and
open a page.

Started from [Karthikeyan-code-byte/virtual-tennis-game](https://github.com/Karthikeyan-code-byte/virtual-tennis-game)
as a reference for the overall shape (ESP32 → WebSocket → browser canvas),
then rebuilt from scratch — see [PLAN.md](PLAN.md) for the reasoning on what
was kept and what wasn't.

## How it works

The ESP32 samples the MPU-6050 at 200 Hz, fuses accelerometer and gyro into
a roll/pitch estimate that doesn't drift and doesn't blow up mid-swing, runs
that through a swing-detection state machine, and streams the result over a
WebSocket at up to 100 Hz. The browser side turns that into paddle position,
runs real ball physics (gravity, bounce, spin, a Magnus force), and renders
it with three.js.

Both the game page and the diagnostic scope are baked into the firmware
itself as PROGMEM strings (`tools/embed_pages.py`), so the ESP32 needs no
SD card or filesystem to serve them — connect to its WiFi and load one IP.

Details, if you want them:

- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the wire format
- [docs/WIRING.md](docs/WIRING.md) — pinout, sensor mounting, power
- [docs/OUTDOOR.md](docs/OUTDOOR.md) — why it lags outdoors and what fixes that

## Hardware

- ESP32 DevKit V1 (30-pin)
- MPU-6050 (GY-521 breakout)
- Something rigid to mount the sensor to, and a powerbank you carry
  separately from the hand piece — see [docs/WIRING.md](docs/WIRING.md) for why.

Six wires, all I2C plus power. Full pinout is in the wiring doc.

## Building and flashing

Uses `arduino-cli`, targeting `esp32:esp32:esp32`.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/air_tt_paddle
arduino-cli upload -p <port> --fqbn esp32:esp32:esp32 firmware/air_tt_paddle
```

If you edit `web/game.html` or `tools/scope.html`, re-embed before flashing:

```bash
python tools/embed_pages.py
```

`tools/prefetch_esp32_core.py` is there if `arduino-cli core install` keeps
dying partway through the download on a flaky connection — it resumes
instead of restarting from zero.

## Running it

1. Flash the firmware, open the serial monitor at 115200 to confirm the IMU
   comes up clean and note the WiFi network it prints.
2. Join that network from your phone or tablet (or point it at your own
   hotspot — see `STA_SSID` in [config.h](firmware/air_tt_paddle/config.h)).
3. Open the printed address, or `http://airtt.local/`.
4. Re-zero from wherever you're holding the paddle, then play.

`tools/scope.html` is the same page structure but plots roll/pitch/gyro/swing
state live instead of running the game — useful for checking the sensor is
behaving or for retuning the swing thresholds in `config.h` against your own
swing.

## Status

Telemetry, orientation fusion, swing detection, and a playable game with AI
opponent and scoring all work. Outdoor play was laggy enough to be worth
fixing properly rather than shrugging off — that fix and the reasoning
behind it is [docs/OUTDOOR.md](docs/OUTDOOR.md).
