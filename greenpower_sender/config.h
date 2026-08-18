// ════════════════════════════════════════════════════════════════════
//  SHARED TELEMETRY PACKET
//  Not transmitted anywhere yet — this struct is the future LoRa /
//  ESP-NOW payload shape. Defined here now so a receiver sketch can
//  pull in an identical copy later without redesigning the data model.
//  66 bytes, no padding (packed).
// ════════════════════════════════════════════════════════════════════

#ifndef CONFIG_H
#define CONFIG_H


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
