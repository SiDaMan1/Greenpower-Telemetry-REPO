// ════════════════════════════════════════════════════════════════════
//  GREENPOWER SENDER  —  V3  (breadboard, sensors + LoRa + ESP-NOW)
//  Heltec ESP32-S3 LoRa WiFi V4
//
//  Sensors collected (all point-to-point wiring, no PCB):
//    • GPS NMEA         — Adafruit Ultimate GPS Breakout V3, Serial1
//                          GPIO34 = ESP32 RX (from GPS TX)
//                          GPIO33 = ESP32 TX (to GPS RX)
//    • IMU              — HW-123 (MPU6050-compatible), I2C SDA=17/SCL=18
//    • Temp probe        — DS18B20, 1-Wire digital, GPIO6 (4.7k pull-up to VCC)
//    • ADS1115 (Lonely Binary board), I2C SDA=17/SCL=18, addr 0x48:
//         A0 = motorVolt   — via 5:1 divider (VCC<25V)
//         A1 = battVolt    — via 5:1 divider (VCC<25V)
//         A2 = Vout        — YHDC HSTS016L 100A current sensor output
//         A3 = Vref        — YHDC HSTS016L reference (measured, not assumed)
//    • Motor RPM         — laser-interrupt disc sensor, GPIO4 (CHANGE-edge, polarity-agnostic)
//    • Wheel RPM         — laser-interrupt disc sensor, GPIO3 (CHANGE-edge, polarity-agnostic)
//    • ESC controller    — UART (Serial2), GPIO44(RX)/GPIO43(TX), from
//                          ../esc controller/throttle_controller.ino (ESP32
//                          WROOM-32, TX=GPIO17/RX=GPIO16 on that end).
//                          20 Hz CSV: mode,state,setpointPct,livePct,rampPct
//    • SD card           — local telemetry backup, own SPI bus (separate
//                          from the LoRa radio's), CS=47 SCK=48 MOSI=7
//                          MISO=5. Logs every packet to /LOGnnn.CSV
//                          regardless of whether the LoRa link is up.
//    • RTC               — Elegoo DS1307-V03, own I2C bus (Wire1) on
//                          GPIO1(SDA)/GPIO2(SCL) — NOT the shared IMU/ADS1115
//                          bus (GPIO17/18), because the DS1307 is fixed at
//                          I2C address 0x68, same as the MPU6050-compatible
//                          IMU already on that bus. Stamps each SD log row
//                          with real wall-clock time instead of just millis().
//
//  LoRa TX: SX1262  NSS=8 RST=12 DIO1=14 BUSY=13  SPI SCK=9 MISO=11 MOSI=10
//           Transmits telemetry_packet_t (now includes ESC fields) every
//           ~2s (SF10, real range gain over the original SF7/200ms)
//
//  ESP-NOW TX: to the steering wheel display_receiver, same CSV format as
//              mock_sender — see espNowSend() below. Mode/state/percent
//              fields now come from the real ESC link; fall back to "---"/0
//              placeholders only if no ESC line has ever been parsed.
//
//  NOTE: ADS1115 must be powered from 5V, not 3.3V — the voltage-divider
//  channels can swing up to ~5V (25V input / 5:1) and would clip against
//  a 3.3V supply rail regardless of gain setting.
//
//  Required libraries (install via Arduino Library Manager):
//    • TinyGPS++               (Mikal Hart)
//    • Adafruit MPU6050        (Adafruit)
//    • Adafruit Unified Sensor (Adafruit)
//    • Adafruit ADS1X15        (Adafruit)
//    • OneWire                 (Paul Stoffregen)
//    • DallasTemperature       (Miles Burton)
//    • RadioLib                (jgromes)
//    • SD                      (built into the ESP32 Arduino core — no
//                                separate Library Manager install needed)
//    • RTClib                  (Adafruit) — DS1307 driver
// ════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>
#include <TinyGPSPlus.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RadioLib.h>
#include <string.h>
#include <stdlib.h>
#include <SD.h>
#include <RTClib.h>
#include "config.h"


// ════════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS
// ════════════════════════════════════════════════════════════════════

#define VEXT_CTRL         36   // External sensor power rail (active LOW) — Heltec V4

// LoRa SPI pins (fixed on Heltec V4). NSS/RST/DIO1/BUSY are in config.h
// since a receiver on the same board type would share them.
#define LORA_SCK           9
#define LORA_MISO         11
#define LORA_MOSI         10
// LoRa moved to SF10 (up from SF7) for real range, per explicit request
// after the sender/receiver were found to disconnect at ordinary walking
// distance. NOT SF12 — an earlier SF12 attempt hung the board completely
// during radio.begin() on real hardware (confirmed: zero serial output,
// on multiple physical boards), and the exact cause was never confirmed,
// only reverted. SF10 is a deliberately more conservative step: it sits
// BELOW the symbol-duration threshold (>16ms) that requires RadioLib to
// enable low-data-rate-optimization for SX126x, which is the leading (but
// still unconfirmed) suspect for what actually hung at SF11/SF12 — SF10's
// time-on-air is short enough (~944ms for this packet, computed via
// Semtech's LoRa time-on-air formula: ~100ms preamble + ~844ms payload at
// BW125/CR4:5/explicit header/CRC on/DE=0) that it doesn't need LDRO at
// all. Real range improvement over SF7, without re-gambling on the exact
// mechanism that broke real boards last time. **Test on ONE board with a
// Serial Monitor attached before flashing others** — this has NOT been
// confirmed safe on real hardware, it's a smaller, better-reasoned bet,
// not a proven one.
// LORA_TX_INTERVAL_MS (~2s, real headroom above the ~944ms airtime) is
// separate from ESP-NOW's own interval now — ESP-NOW is a different radio
// (2.4GHz WiFi-based) with no LoRa-style airtime constraint, and the
// steering wheel display it feeds has no reason to slow down just because
// the LoRa base-station link did.
#define LORA_TX_INTERVAL_MS   2000   // ~SF10 time-on-air (~944ms) + headroom
#define ESPNOW_TX_INTERVAL_MS  200   // unchanged — 5 Hz, matches SENSOR_INTERVAL_MS

#define GPS_RX_PIN        34   // ESP32 RX  ← GPS TX
#define GPS_TX_PIN        33   // ESP32 TX  → GPS RX
#define GPS_BAUD        9600   // Adafruit Ultimate GPS Breakout V3 default

#define I2C_SDA_PIN       17   // shared I2C bus: IMU + ADS1115
#define I2C_SCL_PIN       18

#define TEMP_PROBE_PIN     6   // DS18B20 1-Wire data (needs 4.7k pull-up to VCC)

#define ESC_RX_PIN        44   // ESP32 RX  ← ESC controller TX (GPIO17 on that board)
#define ESC_TX_PIN        43   // ESP32 TX  → ESC controller RX (GPIO16 on that board) — unused by the ESC's code today, wired for symmetry
#define ESC_BAUD      115200   // must match throttle_controller.ino's Serial1.begin()

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

// ── SD card (local telemetry log) ────────────────────────────────────
// A SEPARATE SPI bus from the on-board LoRa radio's — the radio's SPI
// (SCK=9/MISO=11/MOSI=10, see config.h) is wired chip-to-chip internally
// to the co-packaged SX1262 and was never broken out to a header pin at
// all, so there's nothing to physically share. These four pins were
// specifically cross-checked against every other pin already in use on
// this board (GPS/I2C/temp/ESC/RPM above, plus the LoRa radio's own
// internal pins) before being chosen — see greenpower_sender/CLAUDE.md's
// SD-card rule for the full pin-conflict check. GPIO47/48 have no other
// function on this board at all; GPIO5/7 are touch/ADC1-capable but that
// doesn't stop them being used as plain digital SPI pins here.
#define SD_CS_PIN         47
#define SD_SCK_PIN        48
#define SD_MOSI_PIN        7
#define SD_MISO_PIN        5

// ── RTC (Elegoo DS1307-V03) ──────────────────────────────────────────
// A SECOND, independent I2C bus — NOT the existing GPIO17/18 bus. The
// DS1307 has a fixed, non-configurable I2C address (0x68), which is the
// exact same address the MPU6050-compatible IMU already answers to on
// that bus — putting both on one bus means one of them silently doesn't
// respond. The ESP32-S3's I2C peripheral goes through the chip's internal
// GPIO matrix (unlike, say, the LoRa radio's SPI pins, which are hardwired
// chip-to-chip) so a second bus can live on any free GPIO pair — GPIO1/2
// were cross-checked against every other pin already in use on this board
// (see SD_*_PIN's own comment above and greenpower_sender/CLAUDE.md) and
// are clean. Uses the ESP32's second hardware I2C controller (TwoWire
// instance #1) via rtcWire below — this is a genuinely separate bus, not
// a software/bit-banged one.
#define RTC_SDA_PIN        1
#define RTC_SCL_PIN        2

// ADS1115 (Lonely Binary board) — I2C address, ADDR pin → GND = 0x48
#define ADS_I2C_ADDR      0x48
#define ADS_MOTOR_V_CH       0   // A0
#define ADS_BATT_V_CH        1   // A1
// A2/A3 read together as a differential pair for the current sensor (see below)


// ════════════════════════════════════════════════════════════════════
//  CALIBRATION CONSTANTS
// ════════════════════════════════════════════════════════════════════

// Voltage dividers — nominally 5:1, rated for VCC < 25 V (divided output maxes ~5 V).
// Two physically separate resistor pairs — even nominally identical resistors
// have tolerance, so each divider gets its own calibrated ratio rather than
// sharing one constant. Calibration procedure is in CLAUDE.md.
static const float VDIV_RATIO_MOTOR = 0.19893f;   // calibrated: sketch=12.97V, multimeter=13.04V
static const float VDIV_RATIO_BATT  = 0.19954f;   // calibrated: sketch=13.02V, multimeter=13.05V

// YHDC HSTS016L 100 A current sensor, read differentially (Vout - Vref)
// on ADS1115 A2/A3, so no zero-offset constant is needed — Vref is
// measured directly every sample instead of assumed.
//   Amps = (Vout - Vref) / CURRENT_SENS
// CURRENT_SENS (V per A) depends on sensor supply voltage — calibrate
// against a known load.
static const float CURRENT_SENS = 0.016f;   // V per A — placeholder, calibrate

// DS18B20 needs no calibration constants — it's a digital sensor with its
// own factory-calibrated ADC, unlike the analog dividers above.


// ════════════════════════════════════════════════════════════════════
//  PERIPHERAL OBJECTS
// ════════════════════════════════════════════════════════════════════

TinyGPSPlus       gps;
Adafruit_MPU6050  mpu;
Adafruit_ADS1115  ads;
OneWire           oneWire(TEMP_PROBE_PIN);
DallasTemperature tempSensor(&oneWire);

// A dedicated second SPI bus (FSPI) for the SD card — the default `SPI`
// object/pins are already claimed by the LoRa radio (SPI.begin() with
// LORA_SCK/MISO/MOSI in setup() below); a second SPIClass instance on its
// own pins is the standard way to run two independent SPI peripherals on
// one ESP32-S3.
SPIClass sdSPI(FSPI);
bool sdReady = false;
File logFile;

// Second hardware I2C bus (the ESP32-S3 has two independent I2C
// controllers) for the RTC — kept separate from the default `Wire` object
// used by the IMU/ADS1115 above, since the DS1307's fixed 0x68 address
// would collide with the IMU on that bus. See RTC_SDA_PIN's own comment.
TwoWire   rtcWire(1);
RTC_DS1307 rtc;
bool       rtcReady = false;

// SX1262 radio (NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool   loraReady   = false;

// ── Async LoRa TX ───────────────────────────────────────────────────
// At SF10 a single transmit's airtime is ~944ms — long enough that a
// blocking radio.transmit() would stall this loop() (GPS parsing, ESC
// UART polling, RPM period calc, and ESP-NOW TX to the steering wheel)
// for nearly a full second every ~2s cycle. Non-blocking async TX avoids
// that regardless of exactly how long a transmit takes — same
// interrupt-driven pattern greenpower_receiver already uses proven on its
// RX side (setPacketReceivedAction), mirrored here for TX instead of RX.
volatile bool loraTxDoneFlag = false;
void IRAM_ATTR setLoraTxFlag() { loraTxDoneFlag = true; }
// Only ever read/written from loop() (unlike loraTxDoneFlag, which the ISR
// also touches) — true from the moment startTransmit() successfully kicks
// off a transmission until finishTransmit() has been called for it.
bool loraTxInFlight = false;

// ESP-NOW peer (steering wheel display)
static const uint8_t PEER_MAC[6] = ESPNOW_PEER_MAC;
bool   espNowReady = false;

static telemetry_packet_t pkt = {};


// ════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════

static const uint32_t SENSOR_INTERVAL_MS   = 200;   // 5 Hz
static const uint32_t RPM_CALC_INTERVAL_MS = 200;   // 5 Hz — matches SENSOR_INTERVAL_MS so RPM isn't stale between packets
static uint32_t lastSensorMs = 0;
static uint32_t lastGyroMs   = 0;
static uint32_t lastRpmMs    = 0;
static uint32_t lastLoraTxMs = 0;
static uint32_t lastEspNowMs = 0;


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
// to cover the divider's ~5V max output without clipping. Each channel
// passes its own calibrated ratio since the two dividers aren't identical.
static float readDividerVoltage(uint8_t channel, float dividerRatio) {
    ads.setGain(GAIN_TWOTHIRDS);              // ±6.144V FSR
    int16_t raw  = ads.readADC_SingleEnded(channel);
    float   vadc = ads.computeVolts(raw);
    return vadc / dividerRatio;               // undo the divider
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
//  SENSOR READER — DS18B20 (GPIO6, 1-Wire digital)
// ════════════════════════════════════════════════════════════════════

// Raw Celsius reading from the last conversion, kept only for the serial
// diagnostic dump — DEVICE_DISCONNECTED_C (-127) here means the sensor
// didn't respond (check the pull-up resistor and data-line wiring).
static float lastTempRawC = 0.0f;

static float readTempF() {
    // requestTemperatures() blocks until conversion finishes — ~750 ms at the
    // default 12-bit resolution, or ~94 ms at 9-bit (set in setup()). Fine at
    // this sketch's 200 ms sensor-poll cadence.
    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);
    lastTempRawC = tempC;

    if (tempC == DEVICE_DISCONNECTED_C) {
        return NAN;   // caller/print sees this as clearly invalid, not a bogus number
    }
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
//  ESC UART PARSER  (Serial2, from ../esc controller/throttle_controller.ino)
//  Format: mode,state,setpointPct,livePct,rampPct
// ════════════════════════════════════════════════════════════════════

struct EscData {
    char  mode[8];       // matches telemetry_packet_t.esc_mode size
    char  state[8];      // matches telemetry_packet_t.esc_state size
    float setpointPct;
    float livePct;
    float rampPct;
    bool  valid;
};

static EscData esc = {};

// String → packet-code lookups — only needed at the point esc.mode/esc.state
// (parsed off the UART line, still real strings — see EscData above) get
// copied into the packed telemetry_packet_t for LoRa TX. Everywhere else in
// this sketch (SD log, ESP-NOW, serial dump) keeps using esc.mode/esc.state
// directly as strings — only the LoRa packet itself needed shrinking. See
// PKT_ESC_MODE_*/PKT_ESC_STATE_* in config.h for the values these map to.
static uint8_t escModeToCode(const char* mode) {
    if (strcmp(mode, "ECO")    == 0) return PKT_ESC_MODE_ECO;
    if (strcmp(mode, "SPORT")  == 0) return PKT_ESC_MODE_SPORT;
    if (strcmp(mode, "NORMAL") == 0) return PKT_ESC_MODE_NORMAL;
    return PKT_ESC_MODE_UNKNOWN;   // unrecognized string — shouldn't happen, but don't guess
}
static uint8_t escStateToCode(const char* state) {
    if (strcmp(state, "IDLE")  == 0) return PKT_ESC_STATE_IDLE;
    if (strcmp(state, "REENG") == 0) return PKT_ESC_STATE_REENG;
    if (strcmp(state, "RAMP")  == 0) return PKT_ESC_STATE_RAMP;
    if (strcmp(state, "HOLD")  == 0) return PKT_ESC_STATE_HOLD;
    return PKT_ESC_STATE_UNKNOWN;
}

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
//  RTC  (Elegoo DS1307-V03, own I2C bus — see RTC_SDA_PIN's own comment)
//  Only used to stamp the SD log with real wall-clock time — nothing in
//  the LoRa/ESP-NOW packet depends on it, so a missing/dead RTC doesn't
//  affect telemetry, only the SD log's timestamp column.
// ════════════════════════════════════════════════════════════════════

static void initRtc() {
    rtcWire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!rtc.begin(&rtcWire)) {
        Serial.println("[WARN] RTC (DS1307) not detected on I2C (0x68, second bus) — SD log will fall back to millis() only");
        return;
    }
    if (!rtc.isrunning()) {
        // First-ever power-up (or a dead backup battery) — the DS1307
        // starts at an arbitrary/zeroed time until told otherwise. Set it
        // once from the PC's clock at compile time; after that its own
        // backup battery keeps it running across power-cycles, so this
        // branch shouldn't normally fire again unless that battery is
        // removed or dies.
        Serial.println("[WARN] RTC not running — setting from compile time (check backup battery if this happens again)");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    rtcReady = true;
    DateTime now = rtc.now();
    Serial.printf("[OK]   RTC (DS1307)  %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
}

// Formats the RTC's current time as "YYYY-MM-DD HH:MM:SS" into buf, or
// "NO_RTC" if the RTC was never detected — keeps the SD log's column count
// consistent either way rather than shifting columns when the RTC is absent.
static void getRtcTimestamp(char* buf, size_t len) {
    if (!rtcReady) {
        snprintf(buf, len, "NO_RTC");
        return;
    }
    DateTime now = rtc.now();
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());
}


// ════════════════════════════════════════════════════════════════════
//  SD CARD LOGGING  (local backup — independent of LoRa/ESP-NOW)
//  A separate, persistent record of everything this sender sees, kept
//  entirely on the vehicle regardless of whether the LoRa link to the
//  base station ever actually connects. Useful on its own (post-run
//  analysis straight off the card) and as a fallback if the radio link
//  drops out mid-run — nothing here depends on LoRa/ESP-NOW being up.
// ════════════════════════════════════════════════════════════════════

// One new file per power-cycle (LOG001.CSV, LOG002.CSV, ...) rather than
// one giant always-appended file — keeps each run's data separate for
// easier post-race analysis, same reasoning telemetry_web's own
// session-boundary logic uses for a similar problem server-side. Scans
// for the first filename that doesn't already exist rather than assuming
// LOG001 is free — the card persists across power cycles, so a fresh
// boot after previous runs needs to find where the last one left off.
static bool openNextLogFile() {
    char name[16];
    for (int i = 1; i <= 999; i++) {
        snprintf(name, sizeof(name), "/LOG%03d.CSV", i);
        if (!SD.exists(name)) {
            logFile = SD.open(name, FILE_WRITE);
            if (!logFile) return false;
            Serial.printf("[OK]   SD log file: %s\n", name);
            return true;
        }
    }
    return false;   // 999 log files already on the card — extremely unlikely, but don't loop forever
}

static void initSdCard() {
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, sdSPI)) {
        Serial.println("[WARN] SD card not detected — local logging disabled (LoRa/ESP-NOW unaffected)");
        return;
    }
    if (!openNextLogFile()) {
        Serial.println("[WARN] SD card present but couldn't open a log file — local logging disabled");
        return;
    }
    // Header row — column order matches the fields written in logToSD()
    // below. timestamp is real wall-clock time from the DS1307 RTC (or
    // "NO_RTC" for every row if it was never detected); millis_ms is this
    // board's own uptime clock, kept alongside it since it's still useful
    // for computing relative timing/intervals within one run regardless of
    // whether the RTC is present or correctly set.
    logFile.println("timestamp,millis_ms,flags,speed_mph,latitude,longitude,hdop,satellites,"
                     "temp_f,batt_volt,motor_volt,current_a,"
                     "roll_deg,pitch_deg,yaw_deg,accel_g,lateral_g,vertical_g,"
                     "motor_rpm,wheel_rpm,"
                     "esc_mode,esc_state,esc_setpoint_pct,esc_live_pct,esc_ramp_pct");
    logFile.flush();
    sdReady = true;
    Serial.println("[OK]   SD card logging active");
}

// Called once per SENSOR_INTERVAL_MS tick (see loop()), right after
// updateSensors() — logs the exact same `pkt` LoRa/ESP-NOW would send,
// so the card's record and the transmitted telemetry never disagree.
// flush()ed on every single write, not batched — at 5Hz this is a small
// amount of data, and the whole point of a local backup is surviving a
// sudden power loss (a real, plausible failure mode for a race vehicle);
// data sitting in an unflushed buffer when power cuts is data this log
// was specifically supposed to protect against losing.
static void logToSD() {
    if (!sdReady) return;
    char ts[24];
    getRtcTimestamp(ts, sizeof(ts));
    // esc.mode/esc.state (not pkt.esc_mode_code/pkt.esc_state_code) — the SD
    // log is local-only, never transmitted, so it can just use the real
    // strings straight from the last parsed ESC line instead of the LoRa
    // packet's compact numeric codes. "---" matches espNowSend()'s own
    // fallback for "no ESC line ever parsed" (mirrors PKT_FLAG_ESC_VALID).
    bool escValid = pkt.flags & PKT_FLAG_ESC_VALID;
    float hdopOut = (pkt.hdop_x10 == PKT_HDOP_NO_FIX) ? 99.9f : (pkt.hdop_x10 / 10.0f);
    logFile.printf(
        "%s,%lu,%u,%.2f,%.6f,%.6f,%.1f,%u,"
        "%.1f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,"
        "%.0f,%.0f,"
        "%s,%s,%.1f,%.1f,%.1f\n",
        ts, (unsigned long)millis(), pkt.flags, pkt.speed_mph, pkt.latitude, pkt.longitude, hdopOut, pkt.satellites,
        pkt.temp_f, pkt.batt_volt, pkt.motor_volt, pkt.current_a,
        pkt.roll_deg, pkt.pitch_deg, pkt.yaw_deg, pkt.accel_g, pkt.lateral_g, pkt.vertical_g,
        pkt.motor_rpm, pkt.wheel_rpm,
        escValid ? esc.mode : "---", escValid ? esc.state : "---",
        pkt.esc_setpoint_pct, pkt.esc_live_pct, pkt.esc_ramp_pct
    );
    logFile.flush();
}


// ════════════════════════════════════════════════════════════════════
//  SENSOR UPDATE  (called at SENSOR_INTERVAL_MS)
// ════════════════════════════════════════════════════════════════════

// Packs a float HDOP into the packet's single-byte HDOP×10 field — see
// PKT_HDOP_NO_FIX's own comment in config.h for why this isn't a float
// anymore. Clamped to 254 (25.4 HDOP) rather than wrapping/overflowing on
// an unexpectedly large value; 255 is reserved for "no fix".
static uint8_t packHdop(float hdop) {
    long v = lroundf(hdop * 10.0f);
    if (v < 0)   v = 0;
    if (v > 254) v = 254;
    return (uint8_t)v;
}

static void updateGps() {
    if (gps.location.isValid() && gps.location.age() < 2000) {
        pkt.latitude   = (float)gps.location.lat();
        pkt.longitude  = (float)gps.location.lng();
        pkt.speed_mph  = (float)gps.speed.mph();
        pkt.hdop_x10   = gps.hdop.isValid() ? packHdop((float)gps.hdop.hdop()) : PKT_HDOP_NO_FIX;
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

    // Raw Unix seconds, not a formatted string — see epoch_time's own
    // comment in config.h for why (smallest possible over-the-air date/time
    // representation; formatting happens receiver-side, off the radio link).
    pkt.epoch_time  = rtcReady ? rtc.now().unixtime() : 0;
    pkt.temp_f      = readTempF();
    pkt.motor_volt  = readDividerVoltage(ADS_MOTOR_V_CH, VDIV_RATIO_MOTOR);
    pkt.batt_volt   = readDividerVoltage(ADS_BATT_V_CH, VDIV_RATIO_BATT);
    pkt.current_a   = readCurrentAmps();
    pkt.motor_rpm   = motorRpm;
    pkt.wheel_rpm   = wheelRpm;
    pkt.flags      |= PKT_FLAG_CUR_VALID;

    // ESC fields — copied in from the last successfully parsed UART line
    // (pollEsc() runs every loop() iteration, independent of this 200ms tick,
    // so this is just picking up whatever's most recent, not triggering a read).
    if (esc.valid) {
        pkt.esc_mode_code    = escModeToCode(esc.mode);
        pkt.esc_state_code   = escStateToCode(esc.state);
        pkt.esc_setpoint_pct = esc.setpointPct;
        pkt.esc_live_pct     = esc.livePct;
        pkt.esc_ramp_pct     = esc.rampPct;
        pkt.flags           |= PKT_FLAG_ESC_VALID;
    } else {
        pkt.esc_mode_code  = PKT_ESC_MODE_UNKNOWN;
        pkt.esc_state_code = PKT_ESC_STATE_UNKNOWN;
        pkt.flags &= ~PKT_FLAG_ESC_VALID;
    }
}


// ════════════════════════════════════════════════════════════════════
//  LORA TX
// ════════════════════════════════════════════════════════════════════

// Kicks off a transmission and returns immediately — does NOT block for the
// ~944ms SF10 airtime. See loraTxDoneFlag's own comment (near the radio's
// declaration) for why. Completion is picked up later by
// checkLoraTxComplete(), called every loop() iteration independent of
// LORA_TX_INTERVAL_MS's own timing.
static void loRaTx() {
    if (!loraReady) return;
    if (loraTxInFlight) {
        // The previous transmission hasn't finished yet — LORA_TX_INTERVAL_MS
        // has real headroom above SF10's actual airtime specifically so this
        // should be rare, not a normal steady-state occurrence. Skip this
        // cycle rather than call startTransmit() on top of an in-progress
        // one (undefined radio state) or block waiting for it.
        Serial.println("  [LoRa] TX skipped — previous transmit still in flight");
        return;
    }

    int state = radio.startTransmit((uint8_t*)&pkt, sizeof(pkt));
    if (state == RADIOLIB_ERR_NONE) {
        loraTxInFlight = true;
    } else {
        Serial.printf("  [LoRa] startTransmit() ERR %d\n", state);
    }
}

// Called every loop() iteration (cheap — one volatile bool check on every
// pass when nothing's pending) so a completed transmission is noticed
// promptly regardless of where LORA_TX_INTERVAL_MS's own next check lands.
static void checkLoraTxComplete() {
    if (!loraTxDoneFlag) return;
    loraTxDoneFlag = false;

    // finishTransmit() is the required cleanup call after an async
    // startTransmit() (puts the radio back into standby, clears IRQ flags)
    // — RadioLib's documented pairing, same idea as the receiver always
    // re-arming startReceive() after every readData(), successful or not.
    int state = radio.finishTransmit();
    loraTxInFlight = false;

    if (state == RADIOLIB_ERR_NONE) {
        Serial.printf("  [LoRa] TX OK  %u bytes  RSSI:%.0f dBm\n",
                      sizeof(pkt), radio.getRSSI());
    } else {
        Serial.printf("  [LoRa] TX ERR %d\n", state);
    }
}


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW TX
//  Format: speed_mph,batV,rpm,amps,mode,state,setpoint%,live%,ramp%
//  Same format as mock_sender, so display_receiver doesn't care which
//  sender is live. mode/state/percent fields come from the real ESC UART
//  link when available; "---"/0 placeholders only if no ESC line has ever
//  been parsed (PKT_FLAG_ESC_VALID not set).
// ════════════════════════════════════════════════════════════════════

static void espNowSend() {
    if (!espNowReady) return;

    bool escValid = pkt.flags & PKT_FLAG_ESC_VALID;

    char payload[128];
    snprintf(payload, sizeof(payload),
        "%.1f,%.2f,%.0f,%.1f,%s,%s,%.1f,%.1f,%.1f",
        pkt.speed_mph,
        pkt.batt_volt,
        pkt.motor_rpm,
        pkt.current_a,
        escValid ? esc.mode  : "---",
        escValid ? esc.state : "---",
        escValid ? pkt.esc_setpoint_pct : 0.0f,
        escValid ? pkt.esc_live_pct     : 0.0f,
        escValid ? pkt.esc_ramp_pct     : 0.0f
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
    Serial.println("\n[BOOT] Greenpower Sender V3 (breadboard, LoRa + ESP-NOW)");

    // GPS
    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[OK]   GPS    Serial1  RX=%d TX=%d @ %d\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // ESC controller
    Serial2.begin(ESC_BAUD, SERIAL_8N1, ESC_RX_PIN, ESC_TX_PIN);
    Serial.printf("[OK]   ESC    Serial2  RX=%d TX=%d @ %d\n",
                  ESC_RX_PIN, ESC_TX_PIN, ESC_BAUD);

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

    // DS18B20 — 9-bit resolution keeps conversion time short (~94 ms) so it
    // doesn't eat into the 200 ms sensor-poll cadence; 12-bit default is ~750 ms.
    tempSensor.begin();
    tempSensor.setResolution(9);
    Serial.printf("[OK]   Temp    DS18B20 pin=GPIO%d  count=%d\n",
                  TEMP_PROBE_PIN, tempSensor.getDeviceCount());

    // ADS1115 — motorVolt(A0) / battVolt(A1) / current sensor(A2-A3 diff)
    if (!ads.begin(ADS_I2C_ADDR, &Wire)) {
        Serial.println("[WARN] ADS1115 not detected on I2C (0x48)");
    } else {
        Serial.printf("[OK]   ADS1115  0x%02X  A0=motorV A1=battV A2/A3=current\n",
                      ADS_I2C_ADDR);
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
        10,                   // spreading factor — SF10, real range gain (was SF7), deliberately NOT SF12 (hung on real hardware last attempt) — see LORA_TX_INTERVAL_MS's own comment
        5,                    // coding rate 4/5
        LORA_SYNC_WORD,       // 0xF3
        LORA_TX_POWER_DBM,    // 22 dBm
        8                     // preamble length
    );
    if (loraState != RADIOLIB_ERR_NONE) {
        Serial.printf("[WARN] SX1262 init failed  code=%d\n", loraState);
    } else {
        radio.setDio2AsRfSwitch(true);   // required on Heltec V4
        radio.setPacketSentAction(setLoraTxFlag);   // async TX completion — see loraTxDoneFlag's own comment for why this can't be a blocking transmit() at SF10's airtime
        loraReady = true;
        Serial.println("[OK]   SX1262  915 MHz  SF10  BW125  22dBm");
    }

    // RTC — separate I2C bus, see RTC_SDA_PIN's own comment for why. Runs
    // before initSdCard() so the very first SD log rows already have a
    // real timestamp instead of "NO_RTC". Non-fatal if missing: initRtc()
    // itself warns and leaves rtcReady false, everything else keeps working.
    initRtc();

    // SD card — separate SPI bus, see SD_*_PIN's own comment for why these
    // specific pins. Non-fatal if missing/failed: initSdCard() itself
    // warns and leaves sdReady false, everything else keeps working.
    initSdCard();

    Serial.println("[RDY]  Sensor loop starting\n");
}


// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════

void loop() {
    // These run every loop iteration — pollGps()/pollEsc() to keep their
    // UART buffers drained, checkLoraTxComplete() so an async LoRa TX
    // finishing mid-cycle (anywhere in its ~944ms SF10 airtime) is noticed
    // promptly rather than only at the next SENSOR_INTERVAL_MS tick.
    pollGps();
    pollEsc();
    checkLoraTxComplete();

    uint32_t now = millis();
    if (now - lastSensorMs < SENSOR_INTERVAL_MS) return;
    lastSensorMs = now;

    updateSensors();
    logToSD();   // every SENSOR_INTERVAL_MS tick (5Hz) — independent of the LoRa/ESP-NOW radios below, see the SD CARD LOGGING section's own comment

    // ── LoRa TX — every LORA_TX_INTERVAL_MS (~2s, SF10 airtime + headroom) ──
    if (now - lastLoraTxMs >= LORA_TX_INTERVAL_MS) {
        lastLoraTxMs = now;
        loRaTx();
    }

    // ── ESP-NOW TX — every ESPNOW_TX_INTERVAL_MS (200ms/5Hz, unchanged) ──
    if (now - lastEspNowMs >= ESPNOW_TX_INTERVAL_MS) {
        lastEspNowMs = now;
        espNowSend();
    }

    // ── Debug dump to USB serial ─────────────────────────────────────
    Serial.println("──────────────────────────────────────────");

    // Timestamp — from the RTC, "NO_RTC" if it was never detected
    char dbgTs[24];
    getRtcTimestamp(dbgTs, sizeof(dbgTs));
    Serial.printf("  Timestamp : %s UTC  (epoch=%lu)\n", dbgTs, (unsigned long)pkt.epoch_time);

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
    if (isnan(pkt.temp_f)) {
        Serial.println("  Temp      : DISCONNECTED (check pull-up + data line wiring)");
    } else {
        Serial.printf("  Temp      : %.1f °F  (%.2f °C raw)\n", pkt.temp_f, lastTempRawC);
    }

    // GPS
    Serial.printf("  GPS valid : %s\n",      (pkt.flags & PKT_FLAG_GPS_VALID) ? "YES" : "NO");
    Serial.printf("  Satellites: %u\n",       pkt.satellites);
    Serial.printf("  Speed     : %.2f mph\n", pkt.speed_mph);
    Serial.printf("  Latitude  : %.6f\n",     pkt.latitude);
    Serial.printf("  Longitude : %.6f\n",     pkt.longitude);
    Serial.printf("  HDOP      : %.1f\n",     (pkt.hdop_x10 == PKT_HDOP_NO_FIX) ? 99.9f : (pkt.hdop_x10 / 10.0f));

    // IMU
    Serial.printf("  IMU valid : %s\n",      (pkt.flags & PKT_FLAG_IMU_VALID) ? "YES" : "NO");
    Serial.printf("  Roll      : %.2f °\n",   pkt.roll_deg);
    Serial.printf("  Pitch     : %.2f °\n",   pkt.pitch_deg);
    Serial.printf("  Yaw       : %.2f °\n",   pkt.yaw_deg);
    Serial.printf("  Accel     : %.3f g\n",   pkt.accel_g);
    Serial.printf("  Lateral   : %.3f g\n",   pkt.lateral_g);
    Serial.printf("  Vertical  : %.3f g\n",   pkt.vertical_g);

    // ESC
    if (pkt.flags & PKT_FLAG_ESC_VALID) {
        Serial.printf("  ESC Mode  : %s  (code=%u)\n",  esc.mode,  pkt.esc_mode_code);
        Serial.printf("  ESC State : %s  (code=%u)\n",  esc.state, pkt.esc_state_code);
        Serial.printf("  Setpoint  : %.1f %%\n", pkt.esc_setpoint_pct);
        Serial.printf("  Live      : %.1f %%\n", pkt.esc_live_pct);
        Serial.printf("  Ramp      : %.1f %%\n", pkt.esc_ramp_pct);
    } else {
        Serial.println("  ESC       : waiting for data...");
    }

    Serial.println("──────────────────────────────────────────\n");
}
