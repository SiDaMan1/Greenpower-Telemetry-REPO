// ════════════════════════════════════════════════════════════════════
//  GREENPOWER SENDER  —  V2  (breadboard data-collection rework)
//  Heltec ESP32-S3 LoRa WiFi V4
//
//  Sensors collected (all point-to-point wiring, no PCB):
//    • GPS NMEA         — Adafruit Ultimate GPS Breakout V3, Serial1
//                          GPIO34 = ESP32 RX (from GPS TX)
//                          GPIO33 = ESP32 TX (to GPS RX)
//    • IMU              — HW-123 (MPU6050-compatible), I2C SDA=17/SCL=18
//    • Temp probe        — analog NTC thermistor, GPIO6 (ADC1_CH5)
//    • ADS1115 (Lonely Binary board), I2C SDA=17/SCL=18, addr 0x48:
//         A0 = motorVolt   — via 5:1 divider (VCC<25V)
//         A1 = battVolt    — via 5:1 divider (VCC<25V)
//         A2 = Vout        — YHDC HSTS016L 100A current sensor output
//         A3 = Vref        — YHDC HSTS016L reference (measured, not assumed)
//    • Motor RPM         — laser-interrupt disc sensor, GPIO4 (CHANGE-edge, polarity-agnostic)
//    • Wheel RPM         — laser-interrupt disc sensor, GPIO3 (CHANGE-edge, polarity-agnostic)
//
//  NOTE: ADS1115 must be powered from 5V, not 3.3V — the voltage-divider
//  channels can swing up to ~5V (25V input / 5:1) and would clip against
//  a 3.3V supply rail regardless of gain setting.
//
//  This build only reads sensors and prints to USB serial. LoRa (on-board
//  SX1262) and ESP-NOW transmission are not wired up yet — telemetry_packet_t
//  in config.h is kept ready for that so the data model doesn't need to
//  change when radio support is added.
//
//  Required libraries (install via Arduino Library Manager):
//    • TinyGPS++               (Mikal Hart)
//    • Adafruit MPU6050        (Adafruit)
//    • Adafruit Unified Sensor (Adafruit)
//    • Adafruit ADS1X15        (Adafruit)
// ════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADS1X15.h>
#include "config.h"


// ════════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS
// ════════════════════════════════════════════════════════════════════

#define VEXT_CTRL         36   // External sensor power rail (active LOW) — Heltec V4

#define GPS_RX_PIN        34   // ESP32 RX  ← GPS TX
#define GPS_TX_PIN        33   // ESP32 TX  → GPS RX
#define GPS_BAUD        9600   // Adafruit Ultimate GPS Breakout V3 default

#define I2C_SDA_PIN       17   // shared I2C bus: IMU + ADS1115
#define I2C_SCL_PIN       18

#define TEMP_PROBE_PIN     6   // ADC1_CH5 — NTC thermistor analog output

#define MOTOR_RPM_PIN      4   // laser-interrupt — motor disc (was wrongly 48 — never wired)
#define WHEEL_RPM_PIN      3   // laser-interrupt — wheel disc (was wrongly 47 — never wired)
                                // GPIO3 is a boot-strapping pin — fine as a normal input
                                // after boot, don't be alarmed by a reading that looks odd
                                // for an instant right at power-up.

// ── RPM disc slots per revolution ────────────────────────────────────
// Count the slots on each disc and update these values.
#define MOTOR_SLOTS_PER_REV  1
#define WHEEL_SLOTS_PER_REV  1

// These are unbranded modules with no datasheet, so idle/triggered polarity
// isn't assumed — see the RPM section below for how that's handled.
// If a sensor's output is open-collector rather than push-pull, it needs an
// external or internal pull resistor to idle at a defined level; try
// INPUT_PULLUP first (idle HIGH, most common for these break-beam modules).
#define RPM_PIN_MODE      INPUT_PULLUP

// ADS1115 (Lonely Binary board) — I2C address, ADDR pin → GND = 0x48
#define ADS_I2C_ADDR      0x48
#define ADS_MOTOR_V_CH       0   // A0
#define ADS_BATT_V_CH        1   // A1
// A2/A3 read together as a differential pair for the current sensor (see below)


// ════════════════════════════════════════════════════════════════════
//  CALIBRATION CONSTANTS
// ════════════════════════════════════════════════════════════════════

// Voltage dividers — 5:1, rated for VCC < 25 V (divided output maxes ~5 V)
// Adjust if your actual resistor pair gives a different ratio.
static const float VDIV_RATIO = 1.0f / 5.0f;

// YHDC HSTS016L 100 A current sensor, read differentially (Vout - Vref)
// on ADS1115 A2/A3, so no zero-offset constant is needed — Vref is
// measured directly every sample instead of assumed.
//   Amps = (Vout - Vref) / CURRENT_SENS
// CURRENT_SENS (V per A) depends on sensor supply voltage — calibrate
// against a known load.
static const float CURRENT_SENS = 0.016f;   // V per A — placeholder, calibrate

// NTC thermistor temp probe (GPIO6) — divider assumed as:
//   3.3V --[NTC_SERIES_R]-- ADC tap --[NTC]-- GND
// If your probe is wired the opposite way (NTC on top), swap the
// numerator/denominator in readTempF() below.
static const float NTC_SERIES_R = 10000.0f;   // ohms, fixed series resistor
static const float NTC_R0       = 10000.0f;   // ohms, NTC resistance at 25°C
static const float NTC_BETA     =  3950.0f;   // NTC beta coefficient
static const float NTC_T0_K     =   298.15f;  // 25°C in Kelvin

// ESP32-S3 internal ADC (temp probe only — voltage/current now go through ADS1115)
static const float ADC_REF_V   = 3.3f;
static const float ADC_MAX     = 4095.0f;


// ════════════════════════════════════════════════════════════════════
//  PERIPHERAL OBJECTS
// ════════════════════════════════════════════════════════════════════

TinyGPSPlus       gps;
Adafruit_MPU6050  mpu;
Adafruit_ADS1115  ads;

static telemetry_packet_t pkt = {};


// ════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════

static const uint32_t SENSOR_INTERVAL_MS   = 200;   // 5 Hz
static const uint32_t RPM_CALC_INTERVAL_MS = 500;   // recalculate RPM every 500 ms
static uint32_t lastSensorMs = 0;
static uint32_t lastGyroMs   = 0;
static uint32_t lastRpmMs    = 0;


// ════════════════════════════════════════════════════════════════════
//  RPM  (laser break-beam interrupt sensors, unbranded — polarity unknown)
//
//  Measured by PERIOD, not by counting edges in a window. Counting edges
//  over a fixed window only gives whole-number resolution — at low pulse
//  rates each extra edge jumps the computed RPM by a large fixed step
//  (with the old windowed-count method: 60 / (RPM_CALC_INTERVAL_MS/1000)
//  per raw edge). Timing the gap between pulses instead gives a smooth,
//  continuous value that updates on every single pulse.
//
//  Edges are still counted with CHANGE (polarity-agnostic — works whether
//  the sensor idles HIGH or LOW). Since one full beam-block event produces
//  two edges, only every OTHER debounced edge marks a full slot-pass
//  boundary; the period between two such boundaries is a genuine full-pulse
//  period regardless of which physical direction (rising/falling) that is.
//  If RPM reads exactly double the real value once you can spin a disc by
//  hand, flip RPM_COUNT_BOTH_EDGES to 0 and pick a single edge instead.
//
//  Debounce is a short, fixed electrical-noise floor only (not meant to
//  filter real slot pulses) — tune down if RPM looks capped at high speed.
// ════════════════════════════════════════════════════════════════════

#define RPM_COUNT_BOTH_EDGES  1          // 1 = CHANGE, every-other-edge = pulse boundary. 0 = single edge = pulse boundary.
#define RPM_SINGLE_EDGE_MODE  FALLING    // used only if RPM_COUNT_BOTH_EDGES == 0
#define RPM_DEBOUNCE_US       1500       // electrical noise floor, not a real-signal filter
#define RPM_STALE_MS           1000      // no pulse for this long = report 0 instead of freezing

volatile uint32_t motorEdges     = 0;    // raw edge count — diagnostics only, not used in RPM math
volatile uint32_t wheelEdges     = 0;
volatile uint8_t  motorParity    = 0;
volatile uint8_t  wheelParity    = 0;
volatile int64_t  motorPulseUs   = 0;    // timestamp of the last pulse boundary
volatile int64_t  wheelPulseUs   = 0;
volatile int64_t  motorPeriodUs  = 0;    // time between the last two pulse boundaries
volatile int64_t  wheelPeriodUs  = 0;

void IRAM_ATTR motorRpmISR() {
    static int64_t lastUs = 0;
    int64_t now = esp_timer_get_time();
    if (now - lastUs < RPM_DEBOUNCE_US) return;
    lastUs = now;
    motorEdges++;

#if RPM_COUNT_BOTH_EDGES
    motorParity ^= 1;
    if (motorParity != 0) return;   // wait for the matching second edge of this pulse
#endif
    if (motorPulseUs != 0) motorPeriodUs = now - motorPulseUs;
    motorPulseUs = now;
}
void IRAM_ATTR wheelRpmISR() {
    static int64_t lastUs = 0;
    int64_t now = esp_timer_get_time();
    if (now - lastUs < RPM_DEBOUNCE_US) return;
    lastUs = now;
    wheelEdges++;

#if RPM_COUNT_BOTH_EDGES
    wheelParity ^= 1;
    if (wheelParity != 0) return;
#endif
    if (wheelPulseUs != 0) wheelPeriodUs = now - wheelPulseUs;
    wheelPulseUs = now;
}

static float    motorRpm = 0.0f;
static float    wheelRpm = 0.0f;
// Raw edge counts since the last dump, kept only for the serial diagnostic
// print — lets you see edges arriving in real time (or confirm they never
// do, which points at wiring/power, not code).
static uint32_t lastMotorEdges = 0;
static uint32_t lastWheelEdges = 0;

static float periodToRpm(int64_t periodUs, int64_t lastPulseUs, uint32_t slotsPerRev) {
    int64_t nowUs = esp_timer_get_time();
    if (lastPulseUs == 0 || periodUs <= 0 || (nowUs - lastPulseUs) > (RPM_STALE_MS * 1000)) {
        return 0.0f;   // never pulsed yet, or stopped — don't freeze on the last speed
    }
    return 60.0e6f / ((float)periodUs * slotsPerRev);
}

static void updateRpm() {
    uint32_t now     = millis();
    uint32_t elapsed = now - lastRpmMs;
    if (elapsed < RPM_CALC_INTERVAL_MS) return;   // this only paces the dump/refresh rate now
    lastRpmMs = now;

    noInterrupts();
    uint32_t  me           = motorEdges;  motorEdges = 0;
    uint32_t  we           = wheelEdges;  wheelEdges = 0;
    int64_t   motorPeriod  = motorPeriodUs;
    int64_t   wheelPeriod  = wheelPeriodUs;
    int64_t   motorLastPulse = motorPulseUs;
    int64_t   wheelLastPulse = wheelPulseUs;
    interrupts();

    lastMotorEdges = me;
    lastWheelEdges = we;

    motorRpm = periodToRpm(motorPeriod, motorLastPulse, MOTOR_SLOTS_PER_REV);
    wheelRpm = periodToRpm(wheelPeriod, wheelLastPulse, WHEEL_SLOTS_PER_REV);
}


// ════════════════════════════════════════════════════════════════════
//  SENSOR READERS — ADS1115 (voltage dividers + current sensor)
// ════════════════════════════════════════════════════════════════════

// Single-ended read on A0/A1 through a 5:1 divider, gain set wide enough
// to cover the divider's ~5V max output without clipping.
static float readDividerVoltage(uint8_t channel) {
    ads.setGain(GAIN_TWOTHIRDS);              // ±6.144V FSR
    int16_t raw  = ads.readADC_SingleEnded(channel);
    float   vadc = ads.computeVolts(raw);
    return vadc / VDIV_RATIO;                 // undo the 5:1 divider
}

// Differential A2(Vout) - A3(Vref) — cancels the sensor's own zero offset
// in hardware instead of relying on an assumed constant.
static float readCurrentAmps() {
    ads.setGain(GAIN_ONE);                    // ±4.096V FSR — better resolution
    int16_t raw   = ads.readADC_Differential_2_3();
    float   vdiff = ads.computeVolts(raw);
    return vdiff / CURRENT_SENS;
}


// ════════════════════════════════════════════════════════════════════
//  SENSOR READER — NTC thermistor (GPIO6, ESP32 internal ADC)
// ════════════════════════════════════════════════════════════════════

static float readTempF() {
    int   raw  = analogRead(TEMP_PROBE_PIN);
    float vout = (raw / ADC_MAX) * ADC_REF_V;
    vout = constrain(vout, 0.001f, ADC_REF_V - 0.001f);   // avoid /0 and log(<=0) at the rails

    float rNtc  = NTC_SERIES_R * vout / (ADC_REF_V - vout);
    float tempK = 1.0f / (1.0f / NTC_T0_K + logf(rNtc / NTC_R0) / NTC_BETA);
    float tempC = tempK - 273.15f;
    return tempC * 1.8f + 32.0f;
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

    pkt.temp_f      = readTempF();
    pkt.motor_volt  = readDividerVoltage(ADS_MOTOR_V_CH);
    pkt.batt_volt   = readDividerVoltage(ADS_BATT_V_CH);
    pkt.current_a   = readCurrentAmps();
    pkt.motor_rpm   = motorRpm;
    pkt.wheel_rpm   = wheelRpm;
    pkt.flags      |= PKT_FLAG_CUR_VALID;
}


// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);  // wait up to 3s for serial monitor
    Serial.println("\n[BOOT] Greenpower Sender V2 (breadboard)");

    // GPS
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[OK]   GPS    Serial1  RX=%d TX=%d @ %d\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // Enable external sensor power rail (GPIO36, active LOW) — Heltec V4
    pinMode(VEXT_CTRL, OUTPUT);
    digitalWrite(VEXT_CTRL, LOW);
    delay(500);                        // give rail plenty of time to stabilise

    // I2C — SDA=17, SCL=18  (IMU + ADS1115)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    delay(100);

    // IMU (HW-123, MPU6050-compatible)
    if (!mpu.begin(MPU6050_I2CADDR_DEFAULT, &Wire)) {
        Serial.println("[WARN] IMU not detected on I2C (0x68)");
    } else {
        mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
        Serial.println("[OK]   IMU (HW-123)");
    }

    // RPM interrupt sensors
    pinMode(MOTOR_RPM_PIN, RPM_PIN_MODE);
    pinMode(WHEEL_RPM_PIN, RPM_PIN_MODE);
#if RPM_COUNT_BOTH_EDGES
    attachInterrupt(digitalPinToInterrupt(MOTOR_RPM_PIN), motorRpmISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(WHEEL_RPM_PIN), wheelRpmISR, CHANGE);
#else
    attachInterrupt(digitalPinToInterrupt(MOTOR_RPM_PIN), motorRpmISR, RPM_SINGLE_EDGE_MODE);
    attachInterrupt(digitalPinToInterrupt(WHEEL_RPM_PIN), wheelRpmISR, RPM_SINGLE_EDGE_MODE);
#endif
    lastRpmMs = millis();
    Serial.printf("[OK]   RPM     motor=GPIO%d(idle=%d)  wheel=GPIO%d(idle=%d)\n",
                  MOTOR_RPM_PIN, digitalRead(MOTOR_RPM_PIN),
                  WHEEL_RPM_PIN, digitalRead(WHEEL_RPM_PIN));

    // Temp probe ADC — 11 dB attenuation for full 0–3.3 V input range
    analogSetPinAttenuation(TEMP_PROBE_PIN, ADC_11db);
    Serial.printf("[OK]   Temp    pin=GPIO%d\n", TEMP_PROBE_PIN);

    // ADS1115 — motorVolt(A0) / battVolt(A1) / current sensor(A2-A3 diff)
    if (!ads.begin(ADS_I2C_ADDR, &Wire)) {
        Serial.println("[WARN] ADS1115 not detected on I2C (0x48)");
    } else {
        Serial.printf("[OK]   ADS1115  0x%02X  A0=motorV A1=battV A2/A3=current\n",
                      ADS_I2C_ADDR);
    }

    Serial.println("[RDY]  Sensor loop starting\n");
}


// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════

void loop() {
    // Runs every loop iteration to keep the GPS buffer drained
    pollGps();

    uint32_t now = millis();
    if (now - lastSensorMs < SENSOR_INTERVAL_MS) return;
    lastSensorMs = now;

    updateSensors();

    // ── Debug dump to USB serial ─────────────────────────────────────
    Serial.println("──────────────────────────────────────────");

    // Power
    Serial.printf("  Motor Volt: %.2f V\n",  pkt.motor_volt);
    Serial.printf("  Batt Volt : %.2f V\n",  pkt.batt_volt);
    Serial.printf("  Current   : %.2f A\n",  pkt.current_a);

    // RPM — raw edge counts + live pin state included for wiring diagnosis.
    // Edges always 0 with the pin state never changing = wiring/power issue,
    // not a code issue.
    Serial.printf("  Motor RPM : %.0f  (raw edges=%lu/%.1fs, pin=%d)\n",
                  pkt.motor_rpm, (unsigned long)lastMotorEdges,
                  RPM_CALC_INTERVAL_MS / 1000.0f, digitalRead(MOTOR_RPM_PIN));
    Serial.printf("  Wheel RPM : %.0f  (raw edges=%lu/%.1fs, pin=%d)\n",
                  pkt.wheel_rpm, (unsigned long)lastWheelEdges,
                  RPM_CALC_INTERVAL_MS / 1000.0f, digitalRead(WHEEL_RPM_PIN));

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

    Serial.println("──────────────────────────────────────────\n");
}
