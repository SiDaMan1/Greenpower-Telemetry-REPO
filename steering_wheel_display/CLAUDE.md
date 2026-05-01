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
> - Bump the version string in `setup()` in `display_receiver.ino`

---

## What This Project Is

An ESP32 dashboard that receives live telemetry over ESP-NOW wireless and renders it on a Waveshare 4.2" Reflective LCD (RLCD). It is one half of a two-sketch system:

| Sketch | Status | Rule |
|--------|--------|------|
| `throttle_controller.ino` (in parent folder) | **FINAL — V17** | **Never edit. Never suggest edits. It is done.** |
| `display_receiver.ino` (this folder) | Active — **V6** | All work happens here |

---

## Files in This Folder

```
display_receiver.ino   ← the sketch — all active development
display_bsp.h          ← Waveshare RLCD driver header (do not modify)
display_bsp.cpp        ← Waveshare RLCD driver implementation (do not modify)
SYSTEM_INFO.md         ← human-readable system reference, kept in sync with the code
CLAUDE.md              ← this file
```

`display_bsp.h` and `display_bsp.cpp` come from the Waveshare demo package. They **must** stay in the same folder as the sketch or it will not compile.

---

## Rules — Follow These on Every Edit

1. **Bump the version string** in `setup()` on every change to `display_receiver.ino`.  
   Current version: **V6**. Next edit makes it V7, and so on.  
   The line is: `Serial.println("Dashboard V6");`

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
ROW 0  y=0..29    Header: [Mode badge] [State badge] [Battery bar]
ROW 1  y=30..209  Gauges: Speed (left) | RPM (right) — divider at x=200
ROW 2  y=210..299 Info:   Amps bar (left) | SET/LIVE/RAMP rows (right) — divider at x=110
```

Key coordinates (do not change without updating SYSTEM_INFO.md):
- Mode badge: `x=3, w=90, h=24`
- State badge: `x=97, w=108, h=24`
- Battery bar: `x=209, w=182, h=24`
- Speed gauge: `CX=97, CY=120, R=72`
- RPM gauge: `CX=303, CY=120, R=72`
- Amps bar: `x=18, y=222, w=72, h=62`
- SET/LIVE/RAMP: `x=114, w=282`

---

## Fonts

| Identifier | Used for |
|------------|---------|
| `FreeMonoBold24pt7b` | Large gauge numbers |
| `FreeMonoBold12pt7b` | Badge text, amp value |
| `FreeMono9pt7b` | Labels, ticks, battery text, SET/LIVE/RAMP rows |

**Font width reference (FreeMonoBold12pt7b):** "NORMAL" ≈ 84px, "RAMPING" ≈ 98px. The badge widths above were chosen specifically to fit these strings. Do not shrink the badges.

---

## Current State

- **V6 is written and ready to flash.**
- Physical screen output has **not yet been confirmed** — earlier tests flashed correctly but showed a blank screen.
- Most likely causes of blank screen: `display_bsp.cpp` missing from folder, or SPI wiring error.
- Confirm both `display_bsp` files are present before any flash attempt.

---

## Version History

| Version | Changes |
|---------|---------|
| V1–V4 | Persistent overlap bugs — badge text wider than badge box, 3-column crowding |
| V5 | Switched to 2-row layout (gauges top, info bottom) |
| V6 | Fixed badge widths, moved battery bar to x=209, fixed RPM "0k" label bug |
