// ════════════════════════════════════════════════════════════════════
//  GREENPOWER SENDER  —  V1
//  Heltec ESP32-S3 LoRa V4 (SX1262 / 915 MHz)
//
//  Sensors collected:
//    • GPS NMEA        — Serial1  GPIO 33(RX) / 34(TX)
//    • MPU-6050 IMU    — I2C      SDA=17 / SCL=18  (shared with OLED)
//    • DS18B20 temp    — 1-Wire   GPIO 47
//    • Voltage divider — ADC      GPIO 1   (R1=100kΩ top, R2=15kΩ bot → max ~26.7V)
//    • YHDC HSTS016L   — ADS1115  I2C 0x48 AIN0  (100 A Hall-effect, 3.3 V supply)
//    • ESC telemetry   — Serial2  GPIO 45(RX) / 46(TX)  115200 baud
//                        format:  MODE,STATE,setpointPct,livePct,rampPct\n
//
//  Required libraries (install via Arduino Library Manager):
//    • TinyGPS++             (Mikal Hart)
//    • Adafruit MPU6050      (Adafruit)
//    • Adafruit Unified Sensor (Adafruit)
//    • Adafruit ADS1X15      (Adafruit)
//    • OneWire               (Paul Stoffregen)
//    • DallasTemperature     (Miles Burton)
//
//  LoRa TX:  stub — implemented in next step.
// ════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Wire.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"


// ════════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS
// ════════════════════════════════════════════════════════════════════

#define GPS_RX_PIN        33
#define GPS_TX_PIN        34
#define ESC_RX_PIN        45
#define ESC_TX_PIN        46
#define DS18B20_PIN       47
#define VOLTAGE_ADC_PIN    1   // ADC1_CH0

#define GPS_BAUD        9600   // change to 115200 if your module is configured that way

// ADS1115 I2C address (ADDR pin → GND = 0x48, VDD = 0x49, SDA = 0x4A, SCL = 0x4B)
#define ADS_I2C_ADDR    0x48
#define ADS_CURRENT_CH     0   // AIN0 — HSTS016L output


// ════════════════════════════════════════════════════════════════════
//  CALIBRATION CONSTANTS
// ════════════════════════════════════════════════════════════════════

// Voltage divider  (R1 = 100 kΩ top, R2 = 15 kΩ bottom → max ≈ 26.7 V at 3.3 V ADC)
// Adjust R values here if you use different resistors.
static const float VDIV_R1     = 100000.0f;
static const float VDIV_R2     =  15000.0f;
static const float VDIV_RATIO  = VDIV_R2 / (VDIV_R1 + VDIV_R2);

// YHDC HSTS016L 100 A current sensor (3.3 V supply, read via ADS1115)
// Vout = CURRENT_ZERO_V + (Amps × CURRENT_SENS)
// 3.3 V supply → zero ≈ 1.65 V, sensitivity ≈ 16 mV/A
// Calibrate both constants against a known load.
static const float CURRENT_ZERO_V = 1.65f;   // V at 0 A
static const float CURRENT_SENS   = 0.016f;  // V per A

// Voltage divider ADC (ESP32 internal, 12-bit)
static const float ADC_REF_V   = 3.3f;
static const float ADC_MAX     = 4095.0f;


// ════════════════════════════════════════════════════════════════════
//  PERIPHERAL OBJECTS
// ════════════════════════════════════════════════════════════════════

TinyGPSPlus       gps;
Adafruit_MPU6050  mpu;
Adafruit_ADS1115  ads;
OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensors(&oneWire);

// Serial0 = USB debug
// Serial1 = GPS
// Serial2 = ESC


// ════════════════════════════════════════════════════════════════════
//  DATA STRUCTS
// ════════════════════════════════════════════════════════════════════

struct EscData {
    char  mode[12];      // "ECO" | "NORMAL" | "SPORT"
    char  state[16];     // "IDLE" | "REENGAGING" | "RAMPING" | "HOLDING"
    float setpointPct;
    float livePct;
    float rampPct;
    bool  valid;
};

static EscData            esc = {};
static telemetry_packet_t pkt = {};


// ════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════

static const uint32_t SENSOR_INTERVAL_MS = 200;   // 5 Hz
static uint32_t lastSensorMs = 0;
static uint32_t lastGyroMs   = 0;


// ════════════════════════════════════════════════════════════════════
//  SENSOR READERS
// ════════════════════════════════════════════════════════════════════

static float readBatteryVoltage() {
    int   raw  = analogRead(VOLTAGE_ADC_PIN);
    float vadc = (raw / ADC_MAX) * ADC_REF_V;
    return vadc / VDIV_RATIO;
}

static float readCurrentAmps() {
    int16_t raw  = ads.readADC_SingleEnded(ADS_CURRENT_CH);
    float   vadc = ads.computeVolts(raw);
    return (vadc - CURRENT_ZERO_V) / CURRENT_SENS;
}


// ════════════════════════════════════════════════════════════════════
//  ESC UART PARSER
// ════════════════════════════════════════════════════════════════════

static void parseEscLine(char* line) {
    char* tok = strtok(line, ",");
    if (!tok) return;
    strncpy(esc.mode, tok, sizeof(esc.mode) - 1);
    esc.mode[sizeof(esc.mode) - 1] = '\0';

    tok = strtok(nullptr, ",");
    if (!tok) return;
    strncpy(esc.state, tok, sizeof(esc.state) - 1);
    esc.state[sizeof(esc.state) - 1] = '\0';

    tok = strtok(nullptr, ",");
    if (!tok) return;
    esc.setpointPct = atof(tok);

    tok = strtok(nullptr, ",");
    if (!tok) return;
    esc.livePct = atof(tok);

    tok = strtok(nullptr, ",");
    if (!tok) return;
    esc.rampPct = atof(tok);

    esc.valid = true;
}

static void pollEsc() {
    static char    buf[64];
    static uint8_t idx = 0;

    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n') {
            buf[idx] = '\0';
            parseEscLine(buf);
            idx = 0;
        } else if (c != '\r' && idx < sizeof(buf) - 1) {
            buf[idx++] = c;
        }
    }
}


// ════════════════════════════════════════════════════════════════════
//  GPS POLL  (call every loop — feeds TinyGPS++ incrementally)
// ════════════════════════════════════════════════════════════════════

static void pollGps() {
    while (Serial1.available()) {
        gps.encode(Serial1.read());
    }
}


// ════════════════════════════════════════════════════════════════════
//  SENSOR UPDATE  (called at SENSOR_INTERVAL_MS)
// ════════════════════════════════════════════════════════════════════

static void updateGps() {
    if (gps.location.isValid() && gps.location.age() < 2000) {
        pkt.latitude   = (float)gps.location.lat();
        pkt.longitude  = (float)gps.location.lng();
        pkt.speed_mph  = (float)gps.speed.mph();
        pkt.hdop       = gps.hdop.isValid() ? (float)gps.hdop.hdop() : 99.9f;
        pkt.satellites = (uint8_t)gps.satellites.value();
        pkt.flags     |=  PKT_FLAG_GPS_VALID;
    } else {
        pkt.flags &= ~PKT_FLAG_GPS_VALID;
    }
}

static void updateImu() {
    sensors_event_t accelEvt, gyroEvt, tempEvt;
    if (!mpu.getEvent(&accelEvt, &gyroEvt, &tempEvt)) return;

    float ax = accelEvt.acceleration.x;  // m/s²
    float ay = accelEvt.acceleration.y;
    float az = accelEvt.acceleration.z;

    pkt.accel_g    =  ax / 9.80665f;   // forward / braking
    pkt.lateral_g  =  ay / 9.80665f;   // cornering
    pkt.vertical_g =  az / 9.80665f;   // vertical

    // Static tilt angles from accelerometer (accurate at rest, noisy while moving)
    pkt.roll_deg  = atan2f(ay, az) * 57.29578f;
    pkt.pitch_deg = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.29578f;

    // Yaw from gyro integration — drifts without a magnetometer; reset on power-cycle
    uint32_t now = millis();
    if (lastGyroMs > 0) {
        float dt = (now - lastGyroMs) * 0.001f;
        if (dt < 1.0f) {
            pkt.yaw_deg += gyroEvt.gyro.z * 57.29578f * dt;
        }
    }
    lastGyroMs = now;

    pkt.flags |= PKT_FLAG_IMU_VALID;
}

static void updateSensors() {
    updateGps();
    updateImu();

    // DS18B20 — blocking ~750 ms at 12-bit; use 9-bit (93 ms) for 5 Hz loop
    tempSensors.requestTemperatures();
    float tempC = tempSensors.getTempCByIndex(0);
    if (tempC != DEVICE_DISCONNECTED_C) {
        pkt.temp_f = tempC * 1.8f + 32.0f;
    }

    pkt.voltage   = readBatteryVoltage();
    pkt.current_a = readCurrentAmps();
    pkt.flags    |= PKT_FLAG_CUR_VALID;
}


// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[BOOT] Greenpower Sender V1");

    // GPS
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[OK]   GPS    Serial1  RX=%d TX=%d @ %d\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // ESC
    Serial2.begin(115200, SERIAL_8N1, ESC_RX_PIN, ESC_TX_PIN);
    Serial.printf("[OK]   ESC    Serial2  RX=%d TX=%d @ 115200\n",
                  ESC_RX_PIN, ESC_TX_PIN);

    // I2C — shared by MPU-6050 and onboard OLED (0x3C / 0x68, no conflict)
    Wire.begin(OLED_SDA, OLED_SCL);

    // MPU-6050
    if (!mpu.begin(MPU6050_I2CADDR_DEFAULT, &Wire)) {
        Serial.println("[WARN] MPU-6050 not detected on I2C");
    } else {
        mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
        Serial.println("[OK]   MPU-6050");
    }

    // DS18B20 — set 9-bit resolution to keep conversion time under 100 ms
    tempSensors.begin();
    tempSensors.setResolution(9);
    Serial.printf("[OK]   DS18B20  %d device(s)\n", tempSensors.getDeviceCount());

    // Voltage divider ADC — 11 dB attenuation for full 0–3.3 V input range
    analogSetPinAttenuation(VOLTAGE_ADC_PIN, ADC_11db);
    Serial.printf("[OK]   V-ADC   pin=%d\n", VOLTAGE_ADC_PIN);

    // ADS1115 current ADC — GAIN_ONE = ±4.096 V range (0.125 mV/bit), covers 0–3.3 V
    if (!ads.begin(ADS_I2C_ADDR, &Wire)) {
        Serial.println("[WARN] ADS1115 not detected on I2C");
    } else {
        ads.setGain(GAIN_ONE);
        Serial.printf("[OK]   ADS1115  0x%02X  AIN%d → HSTS016L\n",
                      ADS_I2C_ADDR, ADS_CURRENT_CH);
    }

    Serial.println("[RDY]  Sensor loop starting\n");
}


// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════

void loop() {
    // These two run every loop iteration to keep buffers drained
    pollGps();
    pollEsc();

    uint32_t now = millis();
    if (now - lastSensorMs < SENSOR_INTERVAL_MS) return;
    lastSensorMs = now;

    updateSensors();

    // ── LoRa TX stub ─────────────────────────────────────────────────
    // TODO: RadioLib transmit of pkt + esc fields — next step

    // ── Debug dump to USB serial ─────────────────────────────────────
    Serial.printf("V:%.2f V  I:%.1f A  T:%.1f°F  |  GPS:%u sats  %.1f mph  HDOP:%.1f\n",
                  pkt.voltage, pkt.current_a, pkt.temp_f,
                  pkt.satellites, pkt.speed_mph, pkt.hdop);

    Serial.printf("Roll:%.1f°  Pitch:%.1f°  Yaw:%.1f°  "
                  "aX:%.2fg  aY:%.2fg  aZ:%.2fg\n",
                  pkt.roll_deg, pkt.pitch_deg, pkt.yaw_deg,
                  pkt.accel_g, pkt.lateral_g, pkt.vertical_g);

    if (esc.valid) {
        Serial.printf("ESC [%s / %s]  SP:%.0f%%  LV:%.0f%%  RP:%.0f%%\n\n",
                      esc.mode, esc.state,
                      esc.setpointPct, esc.livePct, esc.rampPct);
    } else {
        Serial.println("ESC waiting for data...\n");
    }
}
