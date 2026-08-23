# Wiring & Physical Build

Board: **ESP32 DevKit V1, 30-pin (DOIT)** — the one with `ESP-32 WiFi+BT SoC Inside
ISM2.4G 802.11 b/g/n` on the shield. Sensor: **MPU-6050 (GY-521)**.

---

## 0. About the "D" pin labels

This board's headers are silkscreened `D21`, `D22`, `D19`, `D23` and so on, with
no bare GPIO numbers anywhere. That looks like it needs a translation table.
It does not:

> **On the DevKit V1, the D number *is* the GPIO number.** `D21` is GPIO 21.
> `D22` is GPIO 22. The `D` is decoration.

This is worth stating plainly because it is genuinely confusing — other boards
that use `D` labels (the WEMOS D1 Mini, anything ESP8266-derived) *do* remap
them, so the habit of distrusting `D` numbers is a reasonable one. Just not here.

The reading order down the two edges:

```
  left edge  (top → bottom)              right edge (top → bottom)
    EN                                     D23
    VP   (GPIO36, input only)              D22   ← SCL
    VN   (GPIO39, input only)              TX0
    D34  (input only)                      RX0
    D35  (input only)                      D21   ← SDA
    D32                                    D19
    D33                                    D18
    D25                                    D5
    D26                                    D17
    D27                                    D16
    D14                                    D4
    D12                                    D2
    D13                                    D15
    GND                                    GND
    VIN  (5V)                              3V3   ← power the MPU here
```

Note that `D21` and `D22` are *not* adjacent — `TX0` and `RX0` sit between them.
Do not count header positions; read the labels.

`D34`, `D35`, `VP` and `VN` are **input-only** and have no pull-ups. Nothing here
needs them, but they are a trap if you ever move a pin.

---

## 1. Connections

Six wires. Five if you skip the interrupt line.

| MPU-6050 (GY-521) | Board silkscreen | Required | Note |
|---|---|---|---|
| VCC | **3V3** | yes | Right edge, bottom corner. **Not `VIN`** — that is 5 V. See below. |
| GND | GND | yes | Either edge has one |
| SDA | **D21** | yes | I2C data, bus runs at 400 kHz |
| SCL | **D22** | yes | I2C clock |
| AD0 | GND | yes | Selects address `0x68`. Left floating it may read `0x69` — and *may* is the problem: a floating pin can drift between the two and give an intermittent fault. Tie it down. |
| INT | D19 | optional | Data-ready interrupt. Phase 0 does not use it; it gets used later if sample jitter turns out to matter. |
| XDA / XCL | — | no | Auxiliary I2C for a magnetometer. Nothing to connect. |

### Watch the GY-521's pin order

The header on the sensor reads, in physical order:

```
VCC   GND   SCL   SDA   XDA   XCL   AD0   INT
```

**`SCL` comes before `SDA`.** Most people expect the opposite and wire the pair
by position rather than by label. A swapped SDA/SCL is the single most common
cause of an I2C scan that finds nothing at all.

### Why 3.3 V and not 5 V

The GY-521 has an onboard regulator, so 5 V will not destroy it. But on most of
these modules the I2C pull-up resistors are tied to the **input** rail, not the
regulated output. Feed it 5 V and your SDA/SCL lines get pulled toward 5 V, over
the ESP32's 3.3 V GPIO limit. It often appears to work, then fails oddly under
heat or load, and it degrades the pin over time.

3.3 V costs nothing here. Use it.

---

## 2. Sensor orientation — read this before you tape anything down

The firmware maps sensor axes to game axes with fixed constants. If the sensor
moves relative to your hand, no amount of software fixes it. Mount it **rigidly**
and mount it in a **known** direction.

The convention the firmware assumes:

```
                +X  (toward the tip of the blade)
                 ↑
                 │
                 │        +Z  points out of the hitting face,
        ┌────────┴────────┐    i.e. straight at the ball
        │                 │      (out of the page here)
        │   ●  MPU-6050   │
        │      chip up    │
        │                 │
        └────────┬────────┘
                 │
              ═══╪═══   handle
                 │
       +Y ←──────┘  (across the blade, completing a right-handed set)
```

In practice:

1. Take something flat and stiff for a blade — an old paddle, a phone case, a
   piece of foam board or thick cardboard.
2. Lay the GY-521 **flat on the face**, component side up, so the chip looks out
   in the same direction the ball would be struck. That makes `+Z` the face normal.
3. Rotate it so the silkscreened **X arrow points from the handle toward the tip**.
4. Tape it down hard, then tape the wires down too, a couple of centimetres from
   the header. Swinging pulls on wires and a tugged wire loosens the board.

**Do not worry about the exact resting angle of your wrist.** The game re-zeroes
in software from whatever pose you hold — that part is handled. What cannot be
handled is the sensor rotating inside its mount.

---

## 3. Power and how you carry it

The ESP32 goes on your hand. **The powerbank does not.**

Swinging a 200 g brick wrecks the swing detection and is unpleasant within about
two minutes. Instead:

- ESP32 + MPU on the hand or the blade — that assembly is only ~15 g.
- A short USB cable running to a powerbank in a pocket, on a belt, or on an armband.
- Leave enough slack that a full swing never pulls the cable taut. A cable that
  yanks at the top of your swing will register as a phantom hit.

### The powerbank cutoff problem

Most powerbanks switch themselves off when the load drops below roughly 50–100 mA,
so they do not sit there draining after a phone is unplugged. An ESP32 with WiFi
active draws about 80–150 mA — right on that boundary.

Symptom: everything works fine, then the game freezes mid-rally for no visible
reason. It looks exactly like a WiFi bug, and you can lose an evening to it.

**Test this in Phase 0.** Run the bring-up sketch on the powerbank for five minutes.
If it drops out:
- use a bank with a "low current" / "trickle" / small-device mode (many have a
  double-tap on the button for this),
- or bleed a little extra current with a resistor across the rail,
- or move to a small LiPo (1000 mAh is plenty for hours) with a TP4056 charger.

---

## 4. Bring-up sequence

1. Wire it per the table, with **nothing powered**.
2. Check VCC and GND twice. Reversing them can kill the MPU instantly, and it is
   the one mistake here that is not recoverable.
3. Plug the ESP32 into the PC over USB.
4. Flash `firmware/phase0_bringup`.
5. Open `tools/scope.html` in Chrome or Edge, source **Serial**, Connect, pick the port.

### What a pass looks like

- The I2C scan finds a device at `0x68`.
- `WHO_AM_I = 0x68`, and the config read-back says `config verified OK`.
- Held still: traces flat, `|a|` sits at about **1.00 g**, `|ω|` near **0 °/s**.
- Swung hard: peaks that look like a real swing, and **clip stays `0 / 0`**.

`|a| = 1.00 g` at rest is the quick proof the accelerometer scale is right — that
is gravity, and it should read one g whichever way you tilt it.

---

## 5. When it does not work

| Symptom | Cause |
|---|---|
| I2C scan finds nothing | Power or swapped SDA/SCL, in that order of likelihood. A completely silent bus is almost never a subtle problem. Confirm 3.3 V actually reaches the module's VCC pin. |
| Device shows at `0x69` | AD0 is floating or tied high. Ground it. The sketch handles either, but a floating pin is an intermittent fault waiting to happen. |
| `WHO_AM_I` is not `0x68` | Probably an MPU-6500/9250 clone sold as a 6050. Usually register-compatible enough to continue; the sketch warns and carries on. |
| Readings jump when you touch the wires | Loose joint or a cold solder. Fix it now — it will present later as random phantom hits. |
| `\|a\|` at rest is not ~1.00 g | The accel range did not take. Check the config read-back lines. |
| Rate reads well under 200 Hz | Serial backpressure. Press `s` in the scope's log to pause the stream, or lower the sample rate. |
| Nonzero clip count | The sensor is pinned at its rail during swings. Tell me and I will raise the ranges — until it reads `0 / 0`, swing data is unusable exactly where the game needs it. |
| Freezes after minutes on battery | Powerbank cutoff. See section 3. |
