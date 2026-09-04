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


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW TARGET MAC ADDRESS
//  Set this to the MAC address of the receiving device (display_receiver).
//  Run `WiFi.macAddress()` on the receiver and paste here.
// ════════════════════════════════════════════════════════════════════


#define ESPNOW_PEER_MAC  { 0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4 }


// ════════════════════════════════════════════════════════════════════
//  SHARED TELEMETRY PACKET  (binary, sender → receiver over LoRa)
//  Both sides must include this header — 81 bytes, no padding.
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


typedef struct __attribute__((packed)) {
    uint8_t  flags;         // bit0=GPS valid, bit1=IMU valid, bit2=current valid, bit3=ESC valid
    uint32_t epoch_time;    // Unix timestamp (UTC) from sender's DS1307 RTC, 0 if RTC unavailable.
                             // Sent as a raw 4-byte int, not a formatted string — deliberately the
                             // smallest possible over-the-air representation of date+time+seconds;
                             // a receiver/dashboard converts it to a human date locally (e.g. JS
                             // `new Date(epoch_time * 1000)`), off the radio link entirely.
    float    speed_mph;
    float    latitude;
    float    longitude;
    uint8_t  hdop_x10;       // HDOP × 10, see PKT_HDOP_NO_FIX above — was a float
    uint8_t  satellites;
    float    temp_f;
    float    batt_volt;     // battery voltage   — ADS1115 A1, 5:1 divider
    float    motor_volt;    // motor/ESC voltage — ADS1115 A0, 5:1 divider
    float    current_a;     // motor current     — YHDC HSTS016L, ADS1115 A2(Vout)-A3(Vref)
    float    roll_deg;
    float    pitch_deg;
    float    yaw_deg;
    float    accel_g;
    float    lateral_g;
    float    vertical_g;
    float    motor_rpm;
    float    wheel_rpm;
    uint8_t  esc_mode_code;      // PKT_ESC_MODE_* — was char esc_mode[8]
    uint8_t  esc_state_code;     // PKT_ESC_STATE_* — was char esc_state[8]
    float    esc_setpoint_pct;   // pot target, from ESC controller
    float    esc_live_pct;       // live output %, from ESC controller
    float    esc_ramp_pct;       // ramp/re-engage tracker %, from ESC controller
} telemetry_packet_t;       // 5×uint8 + 1×uint32 + 18×float = 5+4+72 = 81 bytes (was 98 with float hdop + string esc fields)


#endif // CONFIG_H
