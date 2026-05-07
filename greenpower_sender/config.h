// ════════════════════════════════════════════════════════════════════
//  COMMON CONFIGURATION — SHARED BY SENDER & RECEIVER
//  Pin definitions and LoRa settings common to both devices
// ════════════════════════════════════════════════════════════════════


#ifndef CONFIG_H
#define CONFIG_H


// ════════════════════════════════════════════════════════════════════
//  OLED DISPLAY
// ════════════════════════════════════════════════════════════════════


#define OLED_SDA        17
#define OLED_SCL        18
#define OLED_RST        21
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64


// ════════════════════════════════════════════════════════════════════
//  POWER CONTROL
// ════════════════════════════════════════════════════════════════════


#define VEXT_CTRL       36  // External power supply control


// ════════════════════════════════════════════════════════════════════
//  SX1262 LoRa RADIO
// ════════════════════════════════════════════════════════════════════


#define LORA_NSS        8
#define LORA_RST        12
#define LORA_DIO1       14
#define LORA_BUSY       13


// LoRa RF settings
#define LORA_FREQ_MHZ   915.0
#define LORA_SYNC_WORD  0xF3
#define LORA_TX_POWER_DBM 22  // Maximum TX power for SX1262 (Heltec V4)


// ════════════════════════════════════════════════════════════════════
//  SHARED TELEMETRY PACKET  (binary, sender → receiver)
//  Both sides must include this header — 54 bytes, no padding.
// ════════════════════════════════════════════════════════════════════


#define PKT_FLAG_GPS_VALID  0x01
#define PKT_FLAG_IMU_VALID  0x02
#define PKT_FLAG_CUR_VALID  0x04


typedef struct __attribute__((packed)) {
    uint8_t  flags;       // bit 0 = GPS valid, bit 1 = IMU valid, bit 2 = current valid
    float    speed_mph;
    float    latitude;
    float    longitude;
    float    temp_f;
    float    voltage;
    float    current_a;   // battery current (A), from ACS current sensor
    float    hdop;
    uint8_t  satellites;
    float    roll_deg;
    float    pitch_deg;
    float    yaw_deg;
    float    lateral_g;
    float    accel_g;
    float    vertical_g;
} telemetry_packet_t;     // 1+4+4+4+4+4+4+4+1+4+4+4+4+4+4 = 54 bytes


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW TARGET MAC ADDRESS
//  Set this to the MAC address of the receiving device.
//  Run `WiFi.macAddress()` on the receiver and paste here.
// ════════════════════════════════════════════════════════════════════


#define ESPNOW_PEER_MAC  { 0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4 }


#endif // CONFIG_H
