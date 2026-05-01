# Telemetry Dashboard — System Info
**Current version:** V7  
**Last updated:** 2026-05-01

---

## What This Is

Two ESP32 sketches that work together as a wireless telemetry display system for an electric vehicle or motor controller:

| Sketch | Status | Description |
|--------|--------|-------------|
| `throttle_controller.ino` | **FINAL — V17, do not edit** | Throttle controller with OLED display, sends telemetry over ESP-NOW |
| `display_receiver.ino` | **Active development** | Receives telemetry and renders a live dashboard on the RLCD |

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

## Required Files (must all be in the same folder as the sketch)

```
display_receiver.ino   ← main sketch
display_bsp.h          ← Waveshare RLCD driver header (from demo package)
display_bsp.cpp        ← Waveshare RLCD driver implementation
```

If `display_bsp.h` / `display_bsp.cpp` are missing, the sketch will not compile and the display will stay blank.

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
┌──────────────────────────────────────────────────────────────────┐
│ ROW 0  y=0..29     HEADER                                        │
│  [MODE BADGE x=3 w=90] [STATE BADGE x=97 w=108] [BAT BAR x=209] │
├────────────────────────┬─────────────────────────────────────────┤
│ ROW 1  y=30..209       │  GAUGES                                 │
│  SPEED gauge           │  RPM gauge                              │
│  CX=97 CY=120 R=72     │  CX=303 CY=120 R=72                    │
│                        │                                         │
│     (left half 0..199) │ (right half 200..399)                   │
├───────────┬────────────┴─────────────────────────────────────────┤
│ ROW 2     │  INFO BAR  y=210..299                                │
│ AMPS      │  SET / LIVE / RAMP rows                              │
│ x=18 w=72 │  x=114 w=282                                        │
│ (0..109)  │  (110..399)                                          │
└───────────┴─────────────────────────────────────────────────────┘
```

### ROW 0 — Header (y=0..29)
| Widget | x | y | w | h | Notes |
|--------|---|---|---|---|-------|
| Mode badge | 3 | 3 | 90 | 24 | Fits "NORMAL" (≈84px) with FreeMonoBold12pt7b |
| State badge | 97 | 3 | 108 | 24 | Fits "RAMPING" (≈98px) |
| Vehicle battery bar | 209 | 2 | 186 | 12 | Top thin bar — shows `V %`, nub right |
| Screen battery bar | 209 | 16 | 186 | 12 | Bottom thin bar — shows `SCR %`, 18650 via GPIO1 ADC |

Vehicle battery %: **20V = 0%**, **25.6V = 100%** (2× 12V lead-acid in series)  
Screen battery %: **3.0V = 0%**, **4.2V = 100%** (18650)  
Text colour inverts at 50%: white-on-black when >50%, black-on-white when ≤50%.

### ROW 1 — Gauges (y=30..209)
Vertical divider at x=200.

**Speed gauge** (left):
- Range: 0–30 MPH
- Ticks every 5, labels every 10
- No danger zone or efficiency marker

**RPM gauge** (right):
- Range: 0–2500 RPM
- Tick every 250, label every 1000 (displayed as `0` / `1k` / `2k`)
- Danger zone: 2200–2500 (hatched arc)
- Efficiency marker at 1700 RPM (extended tick)

Both gauges share the same `drawGauge()` function with a filled progress arc, needle, hub circle, and large centre number.

### ROW 2 — Info bar (y=210..299)
Vertical divider at x=110.

**Amps section** (left column, 0..109):
- "AMPS" label at top (FreeMono9pt7b)
- Large amp value "36A" centred below label (FreeMonoBold18pt7b)
- Horizontal level bar: x=6, y=253, w=98, h=16
  - Solid fill ≤80A; hatched (checkerboard) fill >80A
  - Vertical warning line at 80A position inside bar
- Scale labels below bar: "0" left, "80" at warn line

**SET / LIVE / RAMP rows** (right column, 110..399):
- Three horizontal mini-bars, one per row
- Label + percentage value printed left of each bar

---

## Fonts

| Font | Size | Used for |
|------|------|---------|
| `FreeMonoBold18pt7b` | ~22px tall | Gauge numbers, amp value |
| `FreeMonoBold12pt7b` | ~14px tall | Badge text |
| `FreeMono9pt7b` | ~10px tall | Labels, tick text, battery bars, SET/LIVE/RAMP rows |

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| V1–V4 | — | Persistent UI overlap: badge text wider than badge box; 3-column layout too crowded |
| V5 | — | Switched to 2-row layout (gauges top, info strip bottom) to fix crowding |
| V6 | 2026-04-30 | Fixed badge widths (mode w=90, state w=108), moved battery bar to x=209, fixed RPM "0k" label bug |
| V7 | 2026-05-01 | Gauge numbers shrunk to FreeMonoBold18pt7b (no arc overlap); RPM end-stop label suppressed; battery bar split into two thin stacked bars (vehicle + 18650 screen); amps redesigned as horizontal bar with large value display; screen battery ADC added on GPIO1 |

---

## Known Issues / Next Steps

- **Screen blank after flash** — not yet confirmed working on physical hardware. Check: (1) `display_bsp.cpp` is in the sketch folder, (2) SPI wiring matches the pin table above.
