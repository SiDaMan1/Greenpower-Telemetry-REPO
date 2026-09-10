// ════════════════════════════════════════════════════════════════════
//  COMMON CONFIGURATION — SHARED BY SENDER & RECEIVER
//  LoRa radio settings and the telemetry packet shape, common to any
//  device that talks to this sender over LoRa or ESP-NOW.
// ════════════════════════════════════════════════════════════════════


#ifndef CONFIG_H
#define CONFIG_H


// ════════════════════════════════════════════════════════════════════
//  SX1262 LoRa RADIO  (on-board, Heltec ESP32-S3 LoRa WiFi V4)
// ════════════════════════════════════════════════════════════════════


#define LORA_NSS        8
#define LORA_RST        12
#define LORA_DIO1       14
#define LORA_BUSY       13


// LoRa RF settings — a receiver must match these to hear this sender
#define LORA_FREQ_MHZ     915.0
#define LORA_SYNC_WORD    0xF3
#define LORA_TX_POWER_DBM 22   // Maximum TX power for SX1262 (Heltec V4)

// Preamble length, in symbols — trimmed from the earlier default of 8 down
// to 6 (Semtech's own documented minimum for reliable SX126x sync) purely
// for latency: at SF7/BW500 each symbol is 0.256ms, so this alone shaves
// ~0.5ms off every single transmission. Going any lower than 6 risks the
// receiver occasionally missing a packet's sync word entirely — 6 is the
// floor generally considered safe, not an arbitrary smaller number. A
// dropped packet here just means one 200ms telemetry update is late by one
// cycle (the very next transmission repairs it), not a real failure mode,
// so this is a good trade for a telemetry stream that free-runs at 5Hz.
#define LORA_PREAMBLE_SYMBOLS  6


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW TARGET MAC ADDRESS
//  Set this to the MAC address of the receiving device (display_receiver).
//  Run `WiFi.macAddress()` on the receiver and paste here.
// ════════════════════════════════════════════════════════════════════


#define ESPNOW_PEER_MAC  { 0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4 }


// ════════════════════════════════════════════════════════════════════
//  SHARED TELEMETRY PACKET  (binary, sender → receiver over LoRa)
//  Both sides must include this header — 42 bytes, no padding.
// ════════════════════════════════════════════════════════════════════


#define PKT_FLAG_GPS_VALID  0x01
#define PKT_FLAG_IMU_VALID  0x02
#define PKT_FLAG_CUR_VALID  0x04
#define PKT_FLAG_ESC_VALID  0x08   // set once a UART line has actually been parsed from the ESC

// esc_mode_code / esc_state_code — 1-byte enum codes standing in for what
// used to be 8-byte ASCII strings (esc_mode[8]/esc_state[8]), specifically
// to shrink LoRa airtime: at SF10 every payload byte adds real transmission
// time, and a fixed 1-of-N code costs 1 byte on the wire instead of 8 for
// data that's really just one of a handful of known values. Values must
// match ../esc%20controller/throttle_controller.ino's modeName()/stateName()
// exactly — see that file if these ever need to change. 255 = never
// received a valid ESC line (mirrors PKT_FLAG_ESC_VALID being unset).
#define PKT_ESC_MODE_ECO       0   // matches throttle_controller.ino's Mode::ECO
#define PKT_ESC_MODE_NORMAL    1   // matches Mode::NORMAL
#define PKT_ESC_MODE_SPORT     2   // matches Mode::SPORT
#define PKT_ESC_MODE_UNKNOWN 255

#define PKT_ESC_STATE_IDLE     0   // matches throttle_controller.ino's State::IDLE
#define PKT_ESC_STATE_REENG    1   // matches State::REENGAGING ("REENG" truncated)
#define PKT_ESC_STATE_RAMP     2   // matches State::RAMPING ("RAMP" truncated)
#define PKT_ESC_STATE_HOLD     3   // matches State::HOLDING ("HOLD" truncated)
#define PKT_ESC_STATE_UNKNOWN 255

// hdop is sent as HDOP×10 in a single byte (0-254 → HDOP 0.0-25.4, plenty
// of range — GPS fixes are rarely usable in any way much past HDOP ~10) —
// 255 is a sentinel meaning "no fix" (replaces the old 99.9f placeholder,
// which no longer fits once hdop stopped being a float).
#define PKT_HDOP_NO_FIX      255


// ── Fixed-point compression ─────────────────────────────────────────
// Every field below that used to be a 4-byte float and doesn't need
// float's actual dynamic range is now a 2-byte (or 1-byte) scaled
// integer instead — this is what actually shrinks LoRa airtime, since
// Semtech's time-on-air formula scales payload symbol count directly
// with payload byte count. Only latitude/longitude stay float (their
// precision genuinely needs it — see below).
//
// encode (sender): raw_value * SCALE, rounded, clamped to the field's
// int range — use pktEncU16()/pktEncI16()/pktEncPct() below.
// decode (anywhere printing/forwarding a human value): stored / SCALE.
#define PKT_SCALE_SPEED     10.0f     // speed_mph_x10:     0.1 mph  res, 0-6553.5 mph range
#define PKT_SCALE_TEMP      10.0f     // temp_f_x10:        0.1 °F   res
#define PKT_SCALE_VOLT      100.0f    // *_volt_x100:       0.01 V   res, 0-655.35 V range
#define PKT_SCALE_CURRENT   100.0f    // current_a_x100:    0.01 A   res, ±327.67 A range
#define PKT_SCALE_ANGLE     100.0f    // pitch_deg_x100:    0.01°    res, ±327.67° range
#define PKT_SCALE_G         1000.0f   // *_g_x1000:         0.001 g  res, ±32.767 g range
#define PKT_SCALE_WHEEL_RPM 10.0f     // wheel_rpm_x10:     0.1 rpm  res, 0-6553.5 rpm range

// Sentinel for "sensor disconnected" on temp_f_x10 — mirrors PKT_HDOP_NO_FIX
// above; an int16 has no NaN, so this stands in for the old isnan(float) check.
#define PKT_TEMP_NO_READING ((int16_t)-32768)

// Clamp-and-round helpers — shared so the encode step is identical no
// matter which sensor function is feeding it. `static inline` is safe in
// a header included by exactly one .ino/.cpp per side (no ODR issue).
static inline uint16_t pktEncU16(float v, float scale) {
    float raw = v * scale;
    if (raw < 0.0f)      raw = 0.0f;
    if (raw > 65535.0f)  raw = 65535.0f;
    return (uint16_t)(raw + 0.5f);
}
static inline int16_t pktEncI16(float v, float scale) {
    float raw = v * scale;
    if (raw >  32767.0f) raw =  32767.0f;
    if (raw < -32767.0f) raw = -32767.0f;
    return (int16_t)(raw + (raw >= 0.0f ? 0.5f : -0.5f));
}
static inline uint8_t pktEncPct(float v) {   // 0-100%, whole-percent res — plenty for a UI percentage
    if (v < 0.0f)   v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    return (uint8_t)(v + 0.5f);
}

typedef struct __attribute__((packed)) {
    uint8_t  flags;         // bit0=GPS valid, bit1=IMU valid, bit2=current valid, bit3=ESC valid
    uint32_t epoch_time;    // Unix timestamp (UTC) from sender's DS1307 RTC, 0 if RTC unavailable.
                             // Sent as a raw 4-byte int, not a formatted string — deliberately the
                             // smallest possible over-the-air representation of date+time+seconds;
                             // a receiver/dashboard converts it to a human date locally (e.g. JS
                             // `new Date(epoch_time * 1000)`), off the radio link entirely.
    uint16_t speed_mph_x10;  // was float speed_mph — see PKT_SCALE_SPEED
    float    latitude;       // kept float — position precision matters, and lat/lon already only
    float    longitude;      // cost 4 bytes each; there's no cheaper fixed-point win worth the risk
    uint8_t  hdop_x10;       // HDOP × 10, see PKT_HDOP_NO_FIX above — was a float
    uint8_t  satellites;
    int16_t  temp_f_x10;     // was float temp_f — see PKT_SCALE_TEMP / PKT_TEMP_NO_READING
    uint16_t batt_volt_x100;  // battery voltage   — ADS1115 A1, 5:1 divider — see PKT_SCALE_VOLT
    uint16_t motor_volt_x100; // motor/ESC voltage — ADS1115 A0, 5:1 divider
    int16_t  current_a_x100;  // motor current — YHDC HSTS016L, ADS1115 A2(Vout)-A3(Vref); signed,
                               // regen braking can read negative — see PKT_SCALE_CURRENT
    int16_t  pitch_deg_x100;  // roll_deg/yaw_deg removed — not needed, per explicit request
    int16_t  accel_g_x1000;
    int16_t  lateral_g_x1000;
    int16_t  vertical_g_x1000;
    uint16_t motor_rpm;        // whole RPM — plenty of resolution for a race motor, no scale needed
    uint16_t wheel_rpm_x10;    // see PKT_SCALE_WHEEL_RPM
    uint8_t  esc_mode_code;      // PKT_ESC_MODE_* — was char esc_mode[8]
    uint8_t  esc_state_code;     // PKT_ESC_STATE_* — was char esc_state[8]
    uint8_t  esc_setpoint_pct;   // pot target, from ESC controller — whole %, see pktEncPct()
    uint8_t  esc_live_pct;       // live output %, from ESC controller
    uint8_t  esc_ramp_pct;       // ramp/re-engage tracker %, from ESC controller
} telemetry_packet_t;       // flags(1)+epoch(4)+speed(2)+lat(4)+lon(4)+hdop(1)+sat(1)+temp(2)+
                             // battV(2)+motorV(2)+curA(2)+pitch(2)+accel(2)+lat_g(2)+vert_g(2)+
                             // motorRpm(2)+wheelRpm(2)+escMode(1)+escState(1)+setpoint(1)+live(1)+
                             // ramp(1) = 42 bytes (airtime_ms_x10 was added, then removed again per
                             // explicit request — see "Current State" in this folder's CLAUDE.md).


#endif // CONFIG_H
