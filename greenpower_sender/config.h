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
//  Both sides must include this header — 33 bytes, no padding.
// ════════════════════════════════════════════════════════════════════


#define PKT_FLAG_GPS_VALID  0x01
#define PKT_FLAG_IMU_VALID  0x02
#define PKT_FLAG_CUR_VALID  0x04
#define PKT_FLAG_ESC_VALID  0x08   // set once a UART line has actually been parsed from the ESC

// esc_mode is packed into flags bits 4-5 (PKT_ESC_MODE_SHIFT/MASK below) —
// NOT its own byte anymore. Exactly 4 values (0-3) fit exactly 2 bits with
// zero precision loss: ECO/NORMAL/SPORT (3 real values) + UNKNOWN, so this
// costs nothing to pack. esc_state deliberately was NOT packed alongside
// it — it has 4 real values (IDLE/REENG/RAMP/HOLD) PLUS its own UNKNOWN
// case (an unrecognized string received while the ESC line otherwise
// parsed fine — a real, distinct case from "no ESC data at all", see
// escStateToCode() in the .ino), which is 5 total values needing 3 bits;
// mode(2 bits)+state(3 bits) = 5 bits doesn't fit flags' 4 spare bits
// (0-3 are GPS/IMU/CUR/ESC valid) without actually losing that UNKNOWN
// distinction, so esc_state_code stays its own full byte below instead of
// taking a real, if tiny, precision hit to save one more byte.
#define PKT_ESC_MODE_SHIFT  4
#define PKT_ESC_MODE_MASK   (0x03 << PKT_ESC_MODE_SHIFT)   // bits 4-5 of flags
static inline uint8_t pktGetEscMode(uint8_t flags) { return (flags >> PKT_ESC_MODE_SHIFT) & 0x03; }
static inline uint8_t pktSetEscMode(uint8_t flags, uint8_t mode) {
    return (uint8_t)((flags & ~PKT_ESC_MODE_MASK) | ((mode & 0x03) << PKT_ESC_MODE_SHIFT));
}

// esc_mode_code / esc_state_code — small enum codes standing in for what
// used to be 8-byte ASCII strings (esc_mode[8]/esc_state[8]), specifically
// to shrink LoRa airtime: every payload byte adds real transmission time,
// and a fixed 1-of-N code costs far less on the wire than 8 bytes for
// data that's really just one of a handful of known values. Values must
// match ../esc%20controller/throttle_controller.ino's modeName()/stateName()
// exactly — see that file if these ever need to change.
#define PKT_ESC_MODE_ECO       0   // matches throttle_controller.ino's Mode::ECO
#define PKT_ESC_MODE_NORMAL    1   // matches Mode::NORMAL
#define PKT_ESC_MODE_SPORT     2   // matches Mode::SPORT
#define PKT_ESC_MODE_UNKNOWN   3   // was 255 — now must fit 2 packed bits (0-3), see above

#define PKT_ESC_STATE_IDLE     0   // matches throttle_controller.ino's State::IDLE
#define PKT_ESC_STATE_REENG    1   // matches State::REENGAGING ("REENG" truncated)
#define PKT_ESC_STATE_RAMP     2   // matches State::RAMPING ("RAMP" truncated)
#define PKT_ESC_STATE_HOLD     3   // matches State::HOLDING ("HOLD" truncated)
#define PKT_ESC_STATE_UNKNOWN  4   // was 255 — now must fit 3 packed bits (0-4), see below

// satellites + esc_state packed into ONE byte instead of two — per
// explicit request for further LOSSLESS compression. Genuinely zero
// precision loss for the real-world range either field ever produces on
// this hardware: satellites gets 5 bits (0-31 — the Adafruit Ultimate GPS
// V3 on this vehicle is GPS-only, MTK3339 chipset, and realistically
// never tracks more than ~14-16 satellites even with a full sky view;
// 31 is a deliberately generous ceiling above that, not a tight fit).
// esc_state gets the remaining 3 bits (0-4 — IDLE/REENG/RAMP/HOLD +
// UNKNOWN is exactly 5 values, an exact fit with zero spare-value waste,
// same reasoning esc_mode's 2-bit packing into flags already used).
// If a future GPS module or firmware update ever needs true multi-GNSS
// satellite counts above 31 (SBAS/GLONASS/Galileo combined counts CAN
// exceed that on some receivers), this ceiling would need revisiting —
// pktSetSatellites() below clamps rather than silently overflowing, so a
// future receiver upgrade would read a clamped-safe number, never a
// wrapped/garbage one, until this field is widened.
#define PKT_SATELLITES_MASK  0x1F                          // bits 0-4 (0-31)
#define PKT_ESC_STATE_SHIFT  5
#define PKT_ESC_STATE_MASK   (0x07 << PKT_ESC_STATE_SHIFT) // bits 5-7 (0-7, only 0-4 used)
static inline uint8_t pktGetSatellites(uint8_t packed) { return packed & PKT_SATELLITES_MASK; }
static inline uint8_t pktSetSatellites(uint8_t packed, uint8_t satellites) {
    if (satellites > PKT_SATELLITES_MASK) satellites = PKT_SATELLITES_MASK;   // clamp, don't wrap — see comment above
    return (uint8_t)((packed & ~PKT_SATELLITES_MASK) | satellites);
}
static inline uint8_t pktGetEscState(uint8_t packed) { return (packed >> PKT_ESC_STATE_SHIFT) & 0x07; }
static inline uint8_t pktSetEscState(uint8_t packed, uint8_t state) {
    return (uint8_t)((packed & ~PKT_ESC_STATE_MASK) | ((state & 0x07) << PKT_ESC_STATE_SHIFT));
}

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
#define PKT_SCALE_VOLT      100.0f    // batt/motor volt:   0.01 V   res, 0-40.95 V range (12-bit packed — see pktEncVolt12())
#define PKT_SCALE_CURRENT   100.0f    // current_a:         0.01 A   res, 0-163.83 A range, UNSIGNED (14-bit packed — see pktEncCurrent14())
#define PKT_SCALE_G         1000.0f   // *_g_x1000:         0.001 g  res, ±32.767 g range
#define PKT_SCALE_WHEEL_RPM 10.0f     // wheel_rpm_x10:     0.1 rpm  res, 0-6553.5 rpm range
// temp_f and pitch_deg deliberately have NO scale constant — both are
// plain whole-number int8 now (not scaled fixed-point), see their own
// struct comments below for why 1° resolution was an acceptable trade
// to save a byte each.

// Sentinel for "sensor disconnected" on temp_f — mirrors PKT_HDOP_NO_FIX
// above; -128 (INT8_MIN) is a temperature no real sensor on this vehicle
// would ever report, same idea as the old int16 -32768 sentinel just
// resized to fit the new int8 field.
#define PKT_TEMP_NO_READING ((int8_t)-128)

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
// Whole-number int8 encode (no scale factor) — for fields where 1-unit
// resolution is an acceptable trade for one less byte on the wire
// (temp_f, pitch_deg). -127 is used as the clamp floor, not -128, so the
// real clamp range never collides with PKT_TEMP_NO_READING's -128
// sentinel — a genuinely very-cold-but-real reading still encodes
// distinctly from "no sensor."
static inline int8_t pktEncI8(float v) {
    if (v >  127.0f) v =  127.0f;
    if (v < -127.0f) v = -127.0f;
    return (int8_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

// ── batt_volt/motor_volt/current_a: cross-byte bit-packed into 5 bytes ──
// Per explicit follow-up ("compress it more... without losing data"),
// then explicit real-world bounds confirmed by the user for THIS vehicle
// specifically: voltage never exceeds 30V (floor 0V, to also safely cover
// a disconnected/failed sensor reading near 0 — not just "normal driving"
// range), current never exceeds 120A and is ALWAYS positive (no regen
// braking on this vehicle — a real, confirmed correction to this
// struct's earlier "regen can read negative" assumption).
//
// At the user's confirmed 0.01V/0.01A resolution (kept exactly as-is —
// nothing here trades away precision):
//   batt_volt:  0-30V   needs  3000 steps -> 12 bits (0-4095 range = 0-40.95V, real headroom above the stated 30V ceiling)
//   motor_volt: 0-30V   needs  3000 steps -> 12 bits (same)
//   current_a:  0-120A  needs 12000 steps -> 14 bits (0-16383 range = 0-163.83A, real headroom above the stated 120A ceiling)
// 12+12+14 = 38 bits, packed into 5 bytes (40 bits, 2 spare) — was 6
// bytes as three separate uint16_t, so this saves 1 byte with the exact
// same 0.01-unit resolution as before AND with more range headroom than
// the user's own stated real-world ceilings, not less.
//
// Packed as a plain uint64_t (not C struct bitfields — bitfield bit/byte
// order is implementation-defined, unsafe for a wire format shared
// between two independently-compiled devices) then split into 5 raw
// bytes LSB-first; unpacked the same way in reverse. This is the same
// "manual shift/mask on plain integers, never bitfields" approach every
// other packed field in this struct already uses, just spanning multiple
// bytes for the first time instead of one.
#define PKT_VOLT12_MAX     4095    // 12 bits — see table above
#define PKT_CURRENT14_MAX 16383    // 14 bits — see table above
static inline uint16_t pktEncVolt12(float v) {
    float raw = v * PKT_SCALE_VOLT;
    if (raw < 0.0f) raw = 0.0f;
    if (raw > (float)PKT_VOLT12_MAX) raw = (float)PKT_VOLT12_MAX;
    return (uint16_t)(raw + 0.5f);
}
static inline uint16_t pktEncCurrent14(float v) {
    // Clamped to >=0 per the user's explicit confirmation that current is
    // always positive on this vehicle — a transient slightly-negative
    // reading here is sensor noise/offset, not a real regen event, and
    // gets clamped to 0.00A rather than encoded as a spurious negative.
    float raw = v * PKT_SCALE_CURRENT;
    if (raw < 0.0f) raw = 0.0f;
    if (raw > (float)PKT_CURRENT14_MAX) raw = (float)PKT_CURRENT14_MAX;
    return (uint16_t)(raw + 0.5f);
}
static inline void pktPackVoltCur(uint8_t out[5], uint16_t battRaw, uint16_t motorRaw, uint16_t curRaw) {
    uint64_t packed = (uint64_t)battRaw | ((uint64_t)motorRaw << 12) | ((uint64_t)curRaw << 24);
    for (int i = 0; i < 5; i++) out[i] = (uint8_t)(packed >> (8 * i));
}
static inline void pktUnpackVoltCur(const uint8_t in[5], uint16_t* battRaw, uint16_t* motorRaw, uint16_t* curRaw) {
    uint64_t packed = 0;
    for (int i = 0; i < 5; i++) packed |= (uint64_t)in[i] << (8 * i);
    *battRaw  = (uint16_t)( packed        & 0x0FFF);
    *motorRaw = (uint16_t)((packed >> 12) & 0x0FFF);
    *curRaw   = (uint16_t)((packed >> 24) & 0x3FFF);
}

typedef struct __attribute__((packed)) {
    uint8_t  flags;         // bit0=GPS valid, bit1=IMU valid, bit2=current valid, bit3=ESC valid,
                             // bits4-5=esc_mode (PKT_ESC_MODE_*, see pktGetEscMode()/pktSetEscMode()
                             // above), bits6-7 unused/free for a future flag.
    // epoch_time (uint32_t, the sender's DS1307 RTC reading) REMOVED per
    // explicit request — the sender still reads its RTC and timestamps
    // the LOCAL SD card log with it (see getRtcTimestamp() in
    // greenpower_sender.ino, unrelated to this struct), but no longer
    // transmits it over LoRa. The dashboard's server already stamps every
    // packet with its own real, NTP-synced clock the instant it arrives
    // (server.js's `received_at`/`now()` — see that file's own comments);
    // that was already the actual source of truth for session timestamps
    // even before this change, so removing this field cost zero real
    // functionality on the server/dashboard side, only the receiver's own
    // serial dump (which had no other clock of its own) lost a "Timestamp"
    // line it can no longer produce.
    uint16_t speed_mph_x10;  // was float speed_mph — see PKT_SCALE_SPEED
    float    latitude;       // kept float — position precision matters, and lat/lon already only
    float    longitude;      // cost 4 bytes each; there's no cheaper fixed-point win worth the risk
    uint8_t  hdop_x10;       // HDOP × 10, see PKT_HDOP_NO_FIX above — was a float
    uint8_t  sat_esc_state;  // satellites (bits 0-4) + esc_state (bits 5-7) packed together — was
                              // two separate bytes; see pktGetSatellites()/pktSetSatellites()/
                              // pktGetEscState()/pktSetEscState() above for the exact split.
    int8_t   temp_f;         // whole °F, was int16 temp_f_x10 — 0.1°F resolution wasn't worth a
                              // second byte for a value nobody reads to a decimal place anyway; see
                              // PKT_TEMP_NO_READING for the "sensor disconnected" sentinel.
    uint8_t  volt_cur_pack[5]; // batt_volt (ADS1115 A1) + motor_volt (ADS1115 A0) + current_a
                                // (YHDC HSTS016L, ADS1115 A2(Vout)-A3(Vref)) all packed together —
                                // was 3 separate uint16_t/int16_t (6 bytes); see
                                // pktEncVolt12()/pktEncCurrent14()/pktPackVoltCur()/
                                // pktUnpackVoltCur() above for the exact 12+12+14-bit layout and
                                // the real-world 0-30V/0-120A bounds it's built on. current_a is
                                // UNSIGNED now — this vehicle has no regen braking, confirmed
                                // explicitly (a real correction to this field's earlier "can read
                                // negative" assumption, not an oversight).
    int8_t   pitch_deg;      // whole degrees, was int16 pitch_deg_x100 — same reasoning as temp_f;
                              // roll_deg/yaw_deg removed entirely — not needed, per explicit request
    int16_t  accel_g_x1000;
    int16_t  lateral_g_x1000;
    int16_t  vertical_g_x1000;
    uint16_t motor_rpm;        // whole RPM — plenty of resolution for a race motor, no scale needed
    uint16_t wheel_rpm_x10;    // see PKT_SCALE_WHEEL_RPM
    uint8_t  esc_setpoint_pct;   // pot target, from ESC controller — whole %, see pktEncPct()
    uint8_t  esc_live_pct;       // live output %, from ESC controller
    uint8_t  esc_ramp_pct;       // ramp/re-engage tracker %, from ESC controller
} telemetry_packet_t;       // flags(1)+speed(2)+lat(4)+lon(4)+hdop(1)+satEscState(1)+temp(1)+
                             // voltCurPack(5)+pitch(1)+accel(2)+lat_g(2)+vert_g(2)+motorRpm(2)+
                             // wheelRpm(2)+setpoint(1)+live(1)+ramp(1) = 33 bytes (was 34 —
                             // batt_volt/motor_volt/current_a's separate 6 bytes replaced with one
                             // 5-byte cross-byte-packed 12+12+14-bit group, per explicit follow-up
                             // to compress further with confirmed real-world bounds — see
                             // pktPackVoltCur()'s own comment above for the exact layout. Still
                             // doesn't cross the Semtech formula's next real symbol-count drop at
                             // 32 bytes — see "Current State" in this folder's CLAUDE.md).


#endif // CONFIG_H
