// ════════════════════════════════════════════════════════════════════
//  COMMON CONFIGURATION — SHARED WITH greenpower_sender
//  Must stay byte-for-byte identical to
//  ../greenpower_sender/config.h for the LoRa RF settings and
//  telemetry_packet_t, or packets sent by that sketch won't decode here.
//  If one changes, change the other in the same commit.
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


// LoRa RF settings — must match greenpower_sender exactly or this receiver
// won't hear it (TX power is meaningless for RX but kept for symmetry with
// the sender's config.h / radio.begin() call signature).
#define LORA_FREQ_MHZ     915.0
#define LORA_SYNC_WORD    0xF3
#define LORA_TX_POWER_DBM 22


// ════════════════════════════════════════════════════════════════════
//  SHARED TELEMETRY PACKET  (binary, sender → receiver over LoRa)
//  Must be identical to greenpower_sender/config.h — 66 bytes, no padding.
// ════════════════════════════════════════════════════════════════════


#define PKT_FLAG_GPS_VALID  0x01
#define PKT_FLAG_IMU_VALID  0x02
#define PKT_FLAG_CUR_VALID  0x04


typedef struct __attribute__((packed)) {
    uint8_t  flags;         // bit0=GPS valid, bit1=IMU valid, bit2=current valid
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
} telemetry_packet_t;       // 2×uint8 + 16×float = 2 + 64 = 66 bytes


#endif // CONFIG_H
