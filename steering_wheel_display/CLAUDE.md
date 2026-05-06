# Project Briefing — Telemetry Dashboard Display Receiver

This file is written for an AI assistant picking up this project cold. Read it before touching any file.

> **AI SELF-MAINTENANCE REQUIREMENT**
> You are responsible for keeping this file and `SYSTEM_INFO.md` up to date.
> After every significant change — new feature, layout edit, bug fix, version bump, or resolved issue —
> you **must** update both files before considering the task complete. This is not optional.
> These files are the only persistent context future AI sessions will have.
> If you skip this step, the next AI starts blind.
>
> Specifically, after each change:
> - Update the `Current version` and `Last updated` line at the top of `SYSTEM_INFO.md`
> - Add a row to the `Version History` table in both files
> - Update any sections whose content changed (layout, wiring, rules, known issues, etc.)
> - Bump the version string in `setup()` in `Steering_Wheel_Display.ino`

---

## What This Project Is

An ESP32 dashboard that receives live telemetry over ESP-NOW wireless and renders it on a Waveshare 4.2" Reflective LCD (RLCD). It is one half of a two-sketch system:

| Sketch | Status | Rule |
|--------|--------|------|
| `throttle_controller.ino` (in parent folder) | **FINAL — V17** | **Never edit. Never suggest edits. It is done.** |
| `Steering_Wheel_Display/Steering_Wheel_Display.ino` | Reference — **V8** | Source of drawing engine |
| `display_receiver/display_receiver.ino` | Active — **V10** | All active development here |

---

## Files in This Folder

```
steering_wheel_display/
├── Steering_Wheel_Display/
│   ├── Steering_Wheel_Display.ino  ← THE SKETCH — all active development
│   ├── display_bsp.h               ← Waveshare RLCD driver header (do not modify)
│   └── display_bsp.cpp             ← Waveshare RLCD driver implementation (do not modify)
├── SYSTEM_INFO.md
└── CLAUDE.md
```

**Arduino IDE rule:** the sketch file name must match its folder name. The folder is `Steering_Wheel_Display` and the sketch is `Steering_Wheel_Display.ino`. Open the `.ino` directly in Arduino IDE — it will handle the folder automatically.

`display_bsp.h` and `display_bsp.cpp` come from the Waveshare demo package. They **must** stay in the same folder as the `.ino` or it will not compile.

---

## Rules — Follow These on Every Edit

1. **Bump the version string** in `setup()` on every change to the active sketch.  
   `display_receiver.ino` is currently **V11**. Next edit makes it V12.  
   The line is: `Serial.println("Dashboard V11");`

2. **Update `SYSTEM_INFO.md`** after any significant change — new feature, layout change, bug fix, or version bump.  
   Update the `Current version`, `Last updated`, and `Version History` table at minimum.  
   Update any section whose facts changed (coordinates, fonts, wiring, known issues, etc.).

3. **Update this file (`CLAUDE.md`)** after any significant change.  
   At minimum: bump the version in the `Current State` section and add a row to `Version History`.  
   If a rule changes or a new constraint is discovered, add it here immediately so the next AI sees it.

4. **Never touch `throttle_controller.ino`**. If a task seems to require it, stop and ask the user.

5. **Never modify `display_bsp.h` or `display_bsp.cpp`**.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | Waveshare ESP32-S3-Zero |
| Display | Waveshare 4.2" RLCD — 400×300px reflective LCD, **not e-paper** |
| SPI: SCK | GPIO 12 |
| SPI: MOSI | GPIO 11 |
| CS | GPIO 5 |
| DC | GPIO 40 |
| RST | GPIO 41 |
| IDE | Arduino 1.8.19, ESP32 core 3.3.8 |
| Upload speed | 460800 (fall back to 115200 if it fails) |

**Flashing:** Sometimes requires manual bootloader — hold BOOT, press RST, release BOOT, then upload.

---

## Display API (from display_bsp.h)

```cpp
DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
RlcdPort.RLCD_Init();
RlcdPort.RLCD_ColorClear(ColorWhite);
RlcdPort.RLCD_SetPixel(x, y, ColorBlack);
RlcdPort.RLCD_Display();
```

All drawing is done on a 1-bit Adafruit GFX off-screen canvas:
```cpp
GFXcanvas1 canvas(400, 300);
```
`pushCanvas()` in the sketch transfers the canvas to the physical display.

---

## ESP-NOW Communication

**Callback signature (ESP32 core 3.x — do not change this signature):**
```cpp
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len)
```

**Incoming packet — comma-separated ASCII string:**
```
speed_mph,batV,rpm,amps,mode,state,setpoint%,live%,ramp%
```

| Field | Type | Notes |
|-------|------|-------|
| speed_mph | float | MPH |
| batV | float | Pack voltage. 20V=0%, 25.6V=100% (2× 12V lead-acid) |
| rpm | float | Motor RPM |
| amps | float | Motor current |
| mode | string ≤7 chars | `NORMAL` / `SPORT` / `ECO` |
| state | string ≤7 chars | `RAMPING` / `HOLD` / `REENG` / `IDLE` |
| setpoint% | float | Throttle setpoint |
| live% | float | Live throttle output |
| ramp% | float | Ramp position |

**Demo mode:** If no packet arrives for >3 seconds the sketch runs animated demo data automatically. This is intentional — do not remove it.

---

## Dashboard Layout

Full pixel-level layout is in `SYSTEM_INFO.md`. Summary:

```
ROW 0   y=0..29    Header: [Mode badge] [State badge] [SCR battery bar]
ROW 1   y=30..219  Gauges: RPM small (left 0..120) | Speed large (120..300) | SET+LIVE mini (300..400)
ROW 2a  y=220..258 AMPS bar (left 0..202) | RAMP bar (right 203..399)
ROW 2b  y=259..299 Vehicle battery bar (full width)
```

Key coordinates (do not change without updating SYSTEM_INFO.md):
- Mode badge: `x=3, w=90, h=24`
- State badge: `x=97, w=108, h=24`
- SCR battery bar: `x=210, w=182, h=17, y=6`
- RPM gauge: `CX=60, CY=132, R=52` — small, left zone
- Speed gauge: `CX=210, CY=132, R=78` — large, centre zone
- SET mini gauge: `CX=350, CY=82, R=32`
- LIVE mini gauge: `CX=350, CY=169, R=32`
- AMPS bar: `x=5, y=234, w=192, h=14` (80A warning line)
- RAMP bar: `x=207, y=234, w=186, h=14`
- Vehicle battery bar: `x=5, y=272, w=389, h=18`

---

## Fonts

| Identifier | Used for |
|------------|---------|
| `FreeMonoBold18pt7b` | Speed gauge number (large centre gauge) |
| `FreeMonoBold12pt7b` | RPM gauge number, mini gauge numbers, badge text |
| `FreeMono9pt7b` | Labels, tick text, battery bars, all bar labels |

**Font width reference (FreeMonoBold12pt7b):** "NORMAL" ≈ 84px, "RAMPING" ≈ 98px. The badge widths above were chosen specifically to fit these strings. Do not shrink the badges.

**numDY parameter in drawGauge():** vertical offset for the centre number — `22` for 18pt (Speed), `12` for 12pt (RPM).

---

## Current State

- **V10 is written, ready to flash.** Active sketch is `display_receiver/display_receiver.ino`.
- All fonts switched to FreeSansBold (9/12/18pt) — sportier sans-serif look.
- `ctClear()` helper draws gauge numbers with a white background rectangle so they never overlap arcs or needle lines.
- RAMP section removed. AMPS bar is now full-width (w=389).
- Vehicle battery is a tall bar (h=30) with "BATT XX.XV XX%" label inside — no separate text label above it.
- Screen battery (18650) read via `analogRead(SCR_BATT_PIN)` on GPIO1 with 1:2 voltage divider even in demo mode. **Verify GPIO1 against schematic** — adjust `SCR_BATT_PIN` / `SCR_BATT_DIV` if needed.
- Charging indicator: `CHRG_PIN = -1` (disabled). Set to TP4056 CHRG pin (active LOW) once identified from schematic/silkscreen.
- `analogSetAttenuation(ADC_11db)` in `setup()` for full 0–3.3V ADC range.

---

## Version History

| Version | Changes |
|---------|---------|
| V1–V4 | Persistent overlap bugs — badge text wider than badge box, 3-column crowding |
| V5 | Switched to 2-row layout (gauges top, info bottom) |
| V6 | Fixed badge widths, moved battery bar to x=209, fixed RPM "0k" label bug |
| V7 | Gauge numbers → FreeMonoBold18pt7b (no arc overlap); RPM end-stop label suppressed; battery bar split into vehicle + 18650 screen bars; amps redesigned as horizontal bar; screen battery ADC on GPIO1 |
| V8 | Full UI redesign: Speed large gauge centred (CX=210,R=78), RPM small gauge left (CX=60,R=52), SET+LIVE mini gauges right column (R=32 each), AMPS + RAMP horizontal bars in bottom strip, vehicle battery bar full-width below that, SCR battery bar top-right, charging indicator support (CHRG_PIN); new helpers: scrBatV/scrBatPct/isCharging/hbar/drawMiniGauge; drawGauge gains numFont+numDY params |
| V9 | Ported V8 to display_receiver.ino (demo-only, no WiFi/ESP-NOW); DashData moved to dash_data.h header |
| V10 | Overlap fixes via ctClear() (white clearbox behind numbers); FreeSansBold fonts throughout; RAMP removed, AMPS full-width; vehicle battery taller bar (h=30) label-inside; battBar vertically centres label; SCR bar h=22; gauge coords tuned |
| V11 | Reduced font sizes: speed gauge 18pt→12pt, RPM gauge 12pt→9pt, badge text 12pt→9pt (fixes overflow in badge box); demo mode SCR battery now simulates 18650 draining 90→20% over 5 min instead of reading garbage ADC; dropped FreeSansBold18pt7b include |
