// ════════════════════════════════════════════════════════════════════
//  GREENPOWER SENDER  —  V1
//  Heltec ESP32-S3 LoRa V4 (SX1262 / 915 MHz)
//
//  Sensors collected:
//    • GPS NMEA        — Serial1  GPIO 33(RX) / 34(TX)
//    • MPU-6050 IMU    — I2C      SDA=17 / SCL=18  (shared with OLED)
//    • DS18B20 temp    — 1-Wire   GPIO 45
//    • Voltage divider — ADC      GPIO 1   (R1=100kΩ top, R2=15kΩ bot → max ~26.7V)
//    • YHDC HSTS016L   — ADS1115  I2C 0x48 AIN0  (100 A Hall-effect, 3.3 V supply)
//    • ESC telemetry   — Serial2  GPIO 44(RX) / 43(TX)  115200 baud
//                        format:  MODE,STATE,setpointPct,livePct,rampPct\n
//
//  LoRa TX: SX1262  NSS=8 RST=12 DIO1=14 BUSY=13  SPI SCK=9 MISO=11 MOSI=10
//           Transmits lora_frame_t every 500 ms  (telemetry_packet_t + ESC fields)
//
//  Required libraries (install via Arduino Library Manager):
//    • TinyGPS++             (Mikal Hart)
//    • Adafruit MPU6050      (Adafruit)
//    • Adafruit Unified Sensor (Adafruit)
//    • Adafruit ADS1X15      (Adafruit)
//    • OneWire               (Paul Stoffregen)
//    • DallasTemperature     (Miles Burton)
//    • RadioLib              (jgromes)
// ════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RadioLib.h>
#include "config.h"


// ════════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS
// ════════════════════════════════════════════════════════════════════

// LoRa SPI pins (fixed on Heltec V4)
#define LORA_SCK          9
#define LORA_MISO        11
#define LORA_MOSI        10

// LoRa TX interval
#define LORA_TX_INTERVAL_MS  500   // 2 Hz

#define GPS_RX_PIN        33
#define GPS_TX_PIN        34
#define ESC_RX_PIN        44
#define ESC_TX_PIN        43
#define DS18B20_PIN       45
#define MOTOR_RPM_PIN     47   // IR/laser interrupt — motor disc
#define WHEEL_RPM_PIN     26   // IR/laser interrupt — wheel disc
#define VOLTAGE_ADC_PIN    1   // ADC1_CH0

// ── RPM disc slots per revolution ────────────────────────────────────
// Count the slots on each disc and update these values.
#define MOTOR_SLOTS_PER_REV  1
#define WHEEL_SLOTS_PER_REV  1

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

// SX1262 radio  (NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool   loraReady    = false;

// ESP-NOW peer (steering wheel display)
static const uint8_t PEER_MAC[6] = ESPNOW_PEER_MAC;
bool   espNowReady  = false;

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

// LoRa frame — telemetry_packet_t + ESC fields in one binary payload
// Receiver must include the same struct to decode.
typedef struct __attribute__((packed)) {
    telemetry_packet_t telem;           // 54 bytes
    char  esc_mode[12];                 // "ECO" | "NORMAL" | "SPORT"
    char  esc_state[16];                // "IDLE" | "REENG" | "RAMP" | "HOLD"
    float esc_setpoint_pct;
    float esc_live_pct;
    float esc_ramp_pct;
    uint8_t esc_valid;
    float   motor_rpm;
    float   wheel_rpm;
} lora_frame_t;                         // 54 + 12 + 16 + 4+4+4 + 1 + 4+4 = 103 bytes

static EscData            esc   = {};
static telemetry_packet_t pkt   = {};
static lora_frame_t       frame = {};


// ════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════

static const uint32_t SENSOR_INTERVAL_MS = 200;   // 5 Hz
static const uint32_t RPM_CALC_INTERVAL_MS = 500; // recalculate RPM every 500 ms
static uint32_t lastSensorMs = 0;
static uint32_t lastGyroMs   = 0;
static uint32_t lastLoraTxMs = 0;
static uint32_t lastEspNowMs = 0;
static uint32_t lastRpmMs    = 0;


// ════════════════════════════════════════════════════════════════════
//  RPM  (interrupt-driven pulse counters)
// ════════════════════════════════════════════════════════════════════

volatile uint32_t motorPulses = 0;
volatile uint32_t wheelPulses = 0;

// 10 ms debounce using esp_timer_get_time() — ISR-safe on ESP32-S3.
// At 3000 RPM / 1 slot = pulse every 20 ms → 2× margin.
// If you add more slots reduce debounce: µs < 60/(maxRPM*slots)*1e6
void IRAM_ATTR motorRpmISR() {
    static int64_t lastUs = 0;
    int64_t now = esp_timer_get_time();
    if (now - lastUs >= 10000) { motorPulses++; lastUs = now; }
}
void IRAM_ATTR wheelRpmISR() {
    static int64_t lastUs = 0;
    int64_t now = esp_timer_get_time();
    if (now - lastUs >= 10000) { wheelPulses++; lastUs = now; }
}

static float motorRpm = 0.0f;
static float wheelRpm = 0.0f;

static void updateRpm() {
    uint32_t now     = millis();
    uint32_t elapsed = now - lastRpmMs;
    if (elapsed < RPM_CALC_INTERVAL_MS) return;
    lastRpmMs = now;

    // Snapshot and reset counters atomically
    noInterrupts();
    uint32_t mp = motorPulses;  motorPulses = 0;
    uint32_t wp = wheelPulses;  wheelPulses = 0;
    interrupts();

    float secs = elapsed / 1000.0f;
    motorRpm = (mp / (float)MOTOR_SLOTS_PER_REV) / secs * 60.0f;
    wheelRpm = (wp / (float)WHEEL_SLOTS_PER_REV) / secs * 60.0f;
}


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
    updateRpm();

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
//  LORA TX
// ════════════════════════════════════════════════════════════════════

static void loRaTx() {
    if (!loraReady) return;

    // Pack latest data into frame
    frame.telem = pkt;
    memcpy(frame.esc_mode,  esc.mode,  sizeof(frame.esc_mode));
    memcpy(frame.esc_state, esc.state, sizeof(frame.esc_state));
    frame.esc_setpoint_pct = esc.setpointPct;
    frame.esc_live_pct     = esc.livePct;
    frame.esc_ramp_pct     = esc.rampPct;
    frame.esc_valid        = esc.valid ? 1 : 0;
    frame.motor_rpm        = motorRpm;
    frame.wheel_rpm        = wheelRpm;

    int state = radio.transmit((uint8_t*)&frame, sizeof(frame));

    if (state == RADIOLIB_ERR_NONE) {
        Serial.printf("  [LoRa] TX OK  %u bytes  RSSI:%.0f dBm\n",
                      sizeof(frame), radio.getRSSI());
    } else {
        Serial.printf("  [LoRa] TX ERR %d\n", state);
    }
}


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW TX
//  Format: speed_mph,batV,rpm,amps,mode,state,setpoint%,live%,ramp%
//  rpm is 0 until a wheel/motor encoder is added.
// ════════════════════════════════════════════════════════════════════

static void espNowSend() {
    if (!espNowReady) return;

    char payload[128];
    snprintf(payload, sizeof(payload),
        "%.1f,%.2f,%.0f,%.1f,%s,%s,%.1f,%.1f,%.1f",
        pkt.speed_mph,
        pkt.voltage,
        motorRpm,
        pkt.current_a,
        esc.valid ? esc.mode  : "---",
        esc.valid ? esc.state : "---",
        esc.setpointPct,
        esc.livePct,
        esc.rampPct
    );

    esp_err_t result = esp_now_send(PEER_MAC, (uint8_t*)payload, strlen(payload));
    Serial.printf("  [ESP-NOW] %s  \"%s\"\n",
                  result == ESP_OK ? "TX OK" : "TX ERR", payload);
}


// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);  // wait up to 3s for serial monitor
    Serial.println("\n[BOOT] Greenpower Sender V1");

    // GPS
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[OK]   GPS    Serial1  RX=%d TX=%d @ %d\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // ESC
    Serial2.begin(115200, SERIAL_8N1, ESC_RX_PIN, ESC_TX_PIN);
    Serial.printf("[OK]   ESC    Serial2  RX=%d TX=%d @ 115200\n",
                  ESC_RX_PIN, ESC_TX_PIN);

    // Enable VEXT power rail (GPIO 36, active LOW) — powers OLED + external sensors
    pinMode(VEXT_CTRL, OUTPUT);
    digitalWrite(VEXT_CTRL, LOW);
    delay(500);                        // give rail plenty of time to stabilise

    // Pulse OLED reset (GPIO 21, active LOW)
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
    delay(200);                        // let OLED finish init before I2C scan

    // I2C — SDA=17, SCL=18  (0x3C=OLED, 0x48=ADS1115, 0x68=MPU-6050)
    Wire.begin(OLED_SDA, OLED_SCL);
    delay(100);

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

    // RPM interrupt sensors
    pinMode(MOTOR_RPM_PIN, INPUT_PULLDOWN);
    pinMode(WHEEL_RPM_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(MOTOR_RPM_PIN), motorRpmISR, RISING);
    attachInterrupt(digitalPinToInterrupt(WHEEL_RPM_PIN), wheelRpmISR, RISING);
    lastRpmMs = millis();
    Serial.printf("[OK]   RPM     motor=GPIO%d  wheel=GPIO%d\n",
                  MOTOR_RPM_PIN, WHEEL_RPM_PIN);

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

    // WiFi (STA, no AP connection) + ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("[WARN] ESP-NOW init failed");
    } else {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, PEER_MAC, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println("[WARN] ESP-NOW add peer failed");
        } else {
            espNowReady = true;
            Serial.printf("[OK]   ESP-NOW → %02X:%02X:%02X:%02X:%02X:%02X\n",
                          PEER_MAC[0], PEER_MAC[1], PEER_MAC[2],
                          PEER_MAC[3], PEER_MAC[4], PEER_MAC[5]);
        }
    }

    // SX1262 LoRa radio
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    int loraState = radio.begin(
        LORA_FREQ_MHZ,        // 915.0 MHz
        125.0,                // bandwidth kHz
        7,                    // spreading factor
        5,                    // coding rate 4/5
        LORA_SYNC_WORD,       // 0xF3
        LORA_TX_POWER_DBM,    // 22 dBm
        8                     // preamble length
    );
    if (loraState != RADIOLIB_ERR_NONE) {
        Serial.printf("[WARN] SX1262 init failed  code=%d\n", loraState);
    } else {
        radio.setDio2AsRfSwitch(true);   // required on Heltec V4
        loraReady = true;
        Serial.println("[OK]   SX1262  915 MHz  SF7  BW125  22dBm");
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

    // ── LoRa TX — every LORA_TX_INTERVAL_MS ─────────────────────────
    if (now - lastLoraTxMs >= LORA_TX_INTERVAL_MS) {
        lastLoraTxMs = now;
        loRaTx();
    }

    // ── ESP-NOW TX — every LORA_TX_INTERVAL_MS ───────────────────────
    if (now - lastEspNowMs >= LORA_TX_INTERVAL_MS) {
        lastEspNowMs = now;
        espNowSend();
    }

    // ── Debug dump to USB serial ─────────────────────────────────────
    Serial.println("──────────────────────────────────────────");

    // Power
    Serial.printf("  Voltage   : %.2f V\n",  pkt.voltage);
    Serial.printf("  Current   : %.2f A\n",  pkt.current_a);

    // RPM
    Serial.printf("  Motor RPM : %.0f\n",    motorRpm);
    Serial.printf("  Wheel RPM : %.0f\n",    wheelRpm);

    // Temperature
    Serial.printf("  Temp      : %.1f °F\n", pkt.temp_f);

    // GPS
    Serial.printf("  GPS valid : %s\n",      (pkt.flags & PKT_FLAG_GPS_VALID) ? "YES" : "NO");
    Serial.printf("  Satellites: %u\n",       pkt.satellites);
    Serial.printf("  Speed     : %.2f mph\n", pkt.speed_mph);
    Serial.printf("  Latitude  : %.6f\n",     pkt.latitude);
    Serial.printf("  Longitude : %.6f\n",     pkt.longitude);
    Serial.printf("  HDOP      : %.1f\n",     pkt.hdop);

    // IMU
    Serial.printf("  IMU valid : %s\n",      (pkt.flags & PKT_FLAG_IMU_VALID) ? "YES" : "NO");
    Serial.printf("  Roll      : %.2f °\n",   pkt.roll_deg);
    Serial.printf("  Pitch     : %.2f °\n",   pkt.pitch_deg);
    Serial.printf("  Yaw       : %.2f °\n",   pkt.yaw_deg);
    Serial.printf("  Accel     : %.3f g\n",   pkt.accel_g);
    Serial.printf("  Lateral   : %.3f g\n",   pkt.lateral_g);
    Serial.printf("  Vertical  : %.3f g\n",   pkt.vertical_g);

    // ESC
    if (esc.valid) {
        Serial.printf("  ESC Mode  : %s\n",   esc.mode);
        Serial.printf("  ESC State : %s\n",   esc.state);
        Serial.printf("  Setpoint  : %.1f %%\n", esc.setpointPct);
        Serial.printf("  Live      : %.1f %%\n", esc.livePct);
        Serial.printf("  Ramp      : %.1f %%\n", esc.rampPct);
    } else {
        Serial.println("  ESC       : waiting for data...");
    }

    Serial.println("──────────────────────────────────────────\n");
}
