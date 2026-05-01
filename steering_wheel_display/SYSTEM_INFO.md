# Telemetry Dashboard — System Info
**Current version:** V8  
**Last updated:** 2026-05-01

---

## What This Is

Two ESP32 sketches that work together as a wireless telemetry display system for an electric vehicle or motor controller:

| Sketch | Status | Description |
|--------|--------|-------------|
| `throttle_controller.ino` | **FINAL — V17, do not edit** | Throttle controller with OLED display, sends telemetry over ESP-NOW |
| `Steering_Wheel_Display/Steering_Wheel_Display.ino` | **Active — V8** | Receives telemetry and renders a live dashboard on the RLCD |

---

## Hardware

| Component | Details |
|-----------|---------|
| Board | Waveshare ESP32-S3-Zero |
| Display | Waveshare 4.2" RLCD (Reflective LCD) — 400×300px, **not e-paper** |
| Screen battery | 18650 cell in Waveshare RLCD dev module, monitored via GPIO1 ADC (verify pin from schematic) |
| IDE | Arduino 1.8.19, ESP32 core 3.3.8 |

### SPI Wiring (display_receiver)
| Signal | Pin |
|--------|-----|
| SCK    | 12  |
| MOSI   | 11  |
| CS     |  5  |
| DC     | 40  |
| RST    | 41  |

### Flashing Notes
- Default upload speed: **460800**. Fall back to **115200** if it fails.
- Sometimes requires manual bootloader entry: hold BOOT → press RST → release BOOT → upload.

---

## File Structure

Arduino IDE requires the sketch file name to match its containing folder name.

```
steering_wheel_display/
├── Steering_Wheel_Display/
│   ├── Steering_Wheel_Display.ino  ← active sketch (open this in Arduino IDE)
│   ├── display_bsp.h               ← Waveshare RLCD driver header (do not modify)
│   └── display_bsp.cpp             ← Waveshare RLCD driver implementation (do not modify)
├── SYSTEM_INFO.md                  ← this file — keep in sync with the code
└── CLAUDE.md                       ← AI briefing file — keep in sync with the code
```

If `display_bsp.h` / `display_bsp.cpp` are missing from the `Steering_Wheel_Display/` folder, the sketch will not compile and the display will stay blank.

---

## Communication: ESP-NOW

The display receiver listens for broadcast ESP-NOW packets from the throttle controller.

**Callback signature (ESP32 core 3.x):**
```cpp
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len)
```

**Packet format — comma-separated ASCII string:**
```
speed_mph,batV,rpm,amps,mode,state,setpoint%,live%,ramp%
```

| Field | Type | Example | Notes |
|-------|------|---------|-------|
| speed_mph | float | `26.3` | Vehicle speed in MPH |
| batV | float | `24.6` | Pack voltage |
| rpm | float | `1700` | Motor RPM |
| amps | float | `36.0` | Motor current draw |
| mode | string (7 char max) | `NORMAL` | `NORMAL` / `SPORT` / `ECO` |
| state | string (7 char max) | `RAMPING` | `RAMPING` / `HOLD` / `REENG` / `IDLE` |
| setpoint% | float | `80.0` | Throttle setpoint % |
| live% | float | `80.0` | Live throttle output % |
| ramp% | float | `80.0` | Ramp position % |

**Demo mode:** If no ESP-NOW packet is received for **>3 seconds**, the display switches to animated demo data automatically. Demo cycles through RAMPING → HOLD → REENG → IDLE over 10 s; mode cycles NORMAL → SPORT → ECO every 12 s.

---

## Display Architecture

### Canvas
```cpp
GFXcanvas1 canvas(400, 300);   // 1-bit Adafruit GFX off-screen buffer
DisplayPort RlcdPort(12, 11, 5, 40, 41, 400, 300);
```
All drawing happens on the canvas. `pushCanvas()` transfers it to the physical display:
1. `RLCD_ColorClear(ColorWhite)` — blanks the display
2. Iterates every pixel in the canvas buffer; calls `RLCD_SetPixel(x, y, ColorBlack)` for each set bit
3. `RLCD_Display()` — commits to screen

### Frame Rate
Capped at **60 fps** — `if (now - lastFrm < 16) return;`

---

## Dashboard Layout

```
┌───────────────────────────────────────────────────────────────────┐
│ ROW 0   y=0..29    HEADER                                         │
│  [MODE x=3 w=90] [STATE x=97 w=108]      [SCR BAT x=210 w=182]  │
├────────────┬──────────────────────────┬───────────────────────────┤
│ ROW 1      │  y=30..219               │                           │
│ RPM small  │  SPEED large             │  SET mini  CX=350 CY=82  │
│ CX=60      │  CX=210                  │  LIVE mini CX=350 CY=169 │
│ CY=132 R=52│  CY=132 R=78             │  (R=32 each)              │
│ (0..120)   │  (120..300)              │  (300..400)               │
├──────┬─────┴──────────────────────────┴───────────────────────────┤
│ROW2a │  y=220..258                                                │
│ AMPS │  x=0..202      RAMP x=203..399                            │
│ bar  │  bar                                                        │
├──────┴────────────────────────────────────────────────────────────┤
│ ROW 2b  y=259..299   VEHICLE BATTERY (full width)                 │
└───────────────────────────────────────────────────────────────────┘
```

### ROW 0 — Header (y=0..29)
| Widget | x | y | w | h | Notes |
|--------|---|---|---|---|-------|
| Mode badge | 3 | 3 | 90 | 24 | Fits "NORMAL" (≈84px) with FreeMonoBold12pt7b |
| State badge | 97 | 3 | 108 | 24 | Fits "RAMPING" (≈98px) |
| SCR battery bar | 210 | 6 | 182 | 17 | 18650 screen battery — shows `SCR %` + charging indicator |

Screen battery %: **3.0V = 0%**, **4.2V = 100%** (18650 via GPIO1 ADC, 1:2 divider).  
Charging indicator: drawn if `CHRG_PIN >= 0` and pin reads LOW (TP4056 CHRG active LOW).  
Text colour inverts at 50%: white-on-black when >50%, black-on-white when ≤50%.

### ROW 1 — Gauges (y=30..219)
Three vertical zones divided at x=120 and x=300.

**RPM gauge** (left zone, x=0..120):
- `CX=60, CY=132, R=52` — small gauge, font `FreeMonoBold12pt7b`, numDY=12
- Range: 0–2500 RPM
- Tick every 250, label every 1000 (displayed as `0` / `1k` / `2k`)
- Danger zone: 2200–2500 (hatched arc)
- Efficiency marker at 1700 RPM (extended tick)
- End-stop label suppressed to avoid arc overlap

**Speed gauge** (centre zone, x=120..300):
- `CX=210, CY=132, R=78` — large gauge, font `FreeMonoBold18pt7b`, numDY=22
- Range: 0–30 MPH
- Ticks every 5, labels every 10
- No danger zone or efficiency marker

**SET + LIVE mini gauges** (right zone, x=300..400):
- `drawMiniGauge()` — R=32, no tick marks, fills arc only, label + number centred
- SET: `CX=350, CY=82`, range 0–100%, label "SET"
- LIVE: `CX=350, CY=169`, range 0–100%, label "LIVE"

All gauges use `drawGauge()` (main) or `drawMiniGauge()` with filled progress arc, needle, hub circle, and centre number.

### ROW 2a — Bottom bar (y=220..258)
Vertical divider at x=203.

**AMPS bar** (left, x=0..202):
- "AMPS" label, value (e.g. "36A") printed left of bar
- Bar: `x=5, y=234, w=192, h=14`
- Solid fill ≤80A; hatched fill >80A
- Vertical warning line at 80A position inside bar; "80" label below

**RAMP bar** (right, x=203..399):
- "RAMP" label, value % printed left of bar
- Bar: `x=207, y=234, w=186, h=14`

### ROW 2b — Vehicle battery (y=259..299)
Full-width bar: `x=5, y=272, w=389, h=18`  
Vehicle battery %: **20V = 0%**, **25.6V = 100%** (2× 12V lead-acid in series).  
Shows voltage and % text. Text colour inverts at 50%.

---

## Fonts

| Font | Size | Used for |
|------|------|---------|
| `FreeMonoBold18pt7b` | ~22px tall | Speed gauge centre number |
| `FreeMonoBold12pt7b` | ~14px tall | RPM gauge number, mini gauge numbers, badge text |
| `FreeMono9pt7b` | ~10px tall | Labels, tick text, battery bars, bar labels |

`drawGauge()` takes `numFont` and `numDY` parameters. Use `numDY=22` for 18pt and `numDY=12` for 12pt.

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| V1–V4 | — | Persistent UI overlap: badge text wider than badge box; 3-column layout too crowded |
| V5 | — | Switched to 2-row layout (gauges top, info strip bottom) to fix crowding |
| V6 | 2026-04-30 | Fixed badge widths (mode w=90, state w=108), moved battery bar to x=209, fixed RPM "0k" label bug |
| V7 | 2026-05-01 | Gauge numbers shrunk to FreeMonoBold18pt7b (no arc overlap); RPM end-stop label suppressed; battery bar split into two thin stacked bars (vehicle + 18650 screen); amps redesigned as horizontal bar with large value display; screen battery ADC added on GPIO1 |
| V8 | 2026-05-01 | Full UI redesign: Speed large centre gauge (CX=210,R=78,18pt), RPM small left gauge (CX=60,R=52,12pt), SET+LIVE mini gauges right column (R=32), AMPS+RAMP horizontal bars in ROW 2a, vehicle battery full-width bar in ROW 2b, SCR battery bar top-right (replaces stacked V7 bars), charging indicator support via CHRG_PIN; new helpers: scrBatV/scrBatPct/isCharging/hbar/drawMiniGauge; drawGauge gains numFont+numDY params |

---

## Known Issues / Next Steps

- **V8 not yet flashed** — V7 was confirmed working on physical hardware. V8 is a full UI redesign; flash and verify visually.
- **Screen battery pin unverified** — `SCR_BATT_PIN = 1` (GPIO1) assumed. Verify against Waveshare RLCD 4.2" dev board schematic before trusting readings. Adjust `SCR_BATT_DIV` if divider ratio differs.
- **CHRG_PIN disabled** — `CHRG_PIN = -1`. Identify the TP4056 CHRG pin from the board schematic/silkscreen and set this constant to enable the charging indicator.
- **Screen blank after flash** — Check: (1) `display_bsp.cpp` is in the `Steering_Wheel_Display/` folder, (2) SPI wiring matches the pin table above, (3) manual bootloader entry may be required.
