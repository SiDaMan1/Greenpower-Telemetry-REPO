# Mock Sender — System Reference

## What This Project Is

An Arduino sketch for an ESP32 that transmits simulated telemetry data via **ESP-NOW** (peer-to-peer Wi-Fi, no router required) to a companion `display_receiver` device. Its purpose is to let you develop and test the receiver's display logic without needing live hardware producing real data. It cycles through a realistic 10-second ride scenario on repeat at 20 Hz.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Microcontroller | Any ESP32 (tested/intended for Arduino Nano ESP32 / ESP32-S3) |
| Connectivity | ESP-NOW over Wi-Fi (station mode) |
| No display, no sensors | This device only transmits — no peripherals needed |

> No special upload procedure is documented for this sketch. Standard USB upload.

---

## File List

| File | Description | Editable? |
|------|-------------|-----------|
| `mock_sender.ino` | Main and only sketch file | Yes — primary edit target |
| `SYSTEM_INFO.md` | This document — human-readable reference | Update after significant changes |
| `CLAUDE.md` | AI assistant briefing | Update after significant changes |

---

## How the Code Works

### ESP-NOW Setup

On boot the sketch:
1. Sets Wi-Fi to station mode (`WIFI_STA`)
2. Prints its own MAC address to Serial
3. Initialises ESP-NOW and registers a send callback
4. Adds the receiver as a peer using the MAC in `RECEIVER_MAC`

Packets are sent to a single peer — the `display_receiver` — whose MAC address must be hardcoded before flashing.

### Packet Format

CSV string, sent as raw bytes:

```
speedMph,batV,rpm,amps,mode,state,setpointPct,livePct,rampPct
```

Example:
```
12.34,24.80,1200.0,42.5,NORMAL,RAMPING,80.0,55.3,55.3
```

The field order and format exactly match `parsePacket()` in `display_receiver`.

### Demo Simulation — 10-second Cycle

`buildFrame(ms)` maps `t = (ms % 10000) / 10000.0` to four phases:

| Phase | `t` range | Description |
|-------|-----------|-------------|
| RAMPING | 0.00–0.45 | Accelerates 0→26 mph, rpm 350→1800, amps 8→70, livePct 0→80% |
| HOLD | 0.45–0.65 | Steady cruise at 26 mph, 80% throttle |
| REENG | 0.65–0.72 | Brief burst to 28 mph, 100% throttle |
| IDLE | 0.72–1.00 | Full deceleration back to zero |

**Battery:** Drains from 25.4 V at 2 V/hour simulation rate. Resets to 25.4 V if it drops below 20 V.

**Mode:** Cycles `NORMAL → SPORT → ECO` every 12 seconds independently of the ride phase.

### Send Rate

`SEND_MS = 50` ms → **20 Hz**. The receiver is capped at ~60 fps display refresh; 20 Hz gives a comfortable margin without flooding.

---

## Key Constants / Parameters

| Constant | Value | Meaning |
|----------|-------|---------|
| `RECEIVER_MAC` | `{0x44,0x1B,0xF6,0xCA,0x38,0xE4}` | Target display_receiver MAC — **must match actual device** |
| `SEND_MS` | 50 ms | Transmit interval (20 Hz) |
| Battery drain rate | 2 V/hr simulated | `25.4 - (ms / 3600000.0) * 2.0` |
| Battery reset floor | 20 V | Resets to 25.4 V when crossed |
| Mode cycle period | 12 000 ms | `(ms / 12000) % 3` |
| Packet max length | 128 bytes | `char pkt[128]` |

---

## Version History

| Version | Notes |
|---------|-------|
| V1 | Initial version — no version string present in source |
