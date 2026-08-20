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
//  Both sides must include this header — 94 bytes, no padding.
// ════════════════════════════════════════════════════════════════════


#define PKT_FLAG_GPS_VALID  0x01
#define PKT_FLAG_IMU_VALID  0x02
#define PKT_FLAG_CUR_VALID  0x04
#define PKT_FLAG_ESC_VALID  0x08   // set once a UART line has actually been parsed from the ESC


typedef struct __attribute__((packed)) {
    uint8_t  flags;         // bit0=GPS valid, bit1=IMU valid, bit2=current valid, bit3=ESC valid
    float    speed_mph;
    float    latitude;
    float    longitude;
    float    hdop;
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
    char     esc_mode[8];        // "ECO"/"NORMAL"/"SPORT" — ESC truncates to 7 chars + null
    char     esc_state[8];       // "IDLE"/"REENG"/"RAMP"/"HOLD" — same truncation
    float    esc_setpoint_pct;   // pot target, from ESC controller
    float    esc_live_pct;       // live output %, from ESC controller
    float    esc_ramp_pct;       // ramp/re-engage tracker %, from ESC controller
} telemetry_packet_t;       // 2×uint8 + 16×float + 2×char[8] + 3×float = 2+64+16+12 = 94 bytes


#endif // CONFIG_H
