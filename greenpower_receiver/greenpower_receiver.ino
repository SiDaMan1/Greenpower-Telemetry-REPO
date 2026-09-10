// ════════════════════════════════════════════════════════════════════
//  GREENPOWER RECEIVER  —  V1
//  Heltec ESP32-S3 LoRa WiFi V4 (SX1262 / 915 MHz)
//
//  Pure LoRa → USB serial relay. Listens for telemetry_packet_t frames
//  from ../greenpower_sender and prints them to Serial — no ESP-NOW, no
//  display, no onward radio. Intended as a base-station/laptop-side
//  receiver: plug this board into a PC over USB and read the dump
//  directly, or have another program parse it off the serial port.
//
//  LoRa RX: SX1262  NSS=8 RST=12 DIO1=14 BUSY=13  SPI SCK=9 MISO=11 MOSI=10
//           Same RF settings as the sender (915 MHz, SF7, BW62.5, sync
//           0xF3) — see config.h, which must stay in sync with the
//           sender's copy. History: latency was prioritized over range
//           first (SF7 — lowest value proven safe on this exact hardware;
//           SF10/SF12 APPEARED to hang this board's radio.begin() at
//           BW125 at the time, but that was later confirmed by the user
//           to be a flashing mistake, not a real SF incompatibility —
//           see CLAUDE.md's ✅ CORRECTION note, higher SF is not actually
//           known-risky — + BW500, the explicitly authorized range-for-
//           latency trade at the time). Packet was then compressed
//           73→42 bytes (fixed-point instead of float, implicit header,
//           trimmed preamble — see config.h; a real measured-airtime
//           field was added and then removed again per explicit request,
//           net zero change to packet size), which freed up enough of
//           the 200ms update budget that bandwidth went back down twice
//           more for range: 500→125→62.5, per two direct follow-up
//           requests ("max range without sacrificing an update every
//           200ms"), spending the slack the compression work created
//           instead of accepting either less range or a slower update
//           rate. SF stayed at 7 throughout this whole bandwidth-only
//           path — not because higher SF is risky (it isn't, per the
//           correction above), just because a bandwidth-only step was
//           simpler to reason about and this exact SF7/BW62.5
//           combination costs LESS airtime than the SF12/BW62.5 range
//           attempt further below ever did.
//           ~160ms computed time-on-air per packet now (Semtech-formula
//           estimate — this project no longer measures it directly, see
//           the ⚠️ SEVENTH pass note in greenpower_sender.ino), still
//           under the 200ms update interval (~40ms margin, the practical
//           ceiling for a bandwidth-only push at SF7 within that
//           budget), down from the ~6.9s at the SF12/BW62.5 range
//           attempt.
//
//  Serial protocol for downstream tooling (e.g. receiver_agent):
//    • Boot prints one line containing DEVICE_ID once — lets a host script
//      identify this as a Greenpower receiver among other USB devices.
//    • Sending "ID?\n" at any time gets an immediate DEVICE_ID reply, in
//      case the host connected after the boot line already went by.
//    • Every successfully decoded packet also prints one line prefixed
//      "JSON:" with a compact machine-readable version of the same data
//      shown in the human dump above it. Parse that line, not the dump.
//
//  Required libraries (install via Arduino Library Manager):
//    • RadioLib   (jgromes)
// ════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <string.h>
#include <time.h>
#include "config.h"


// ════════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS
// ════════════════════════════════════════════════════════════════════

// LoRa SPI pins (fixed on Heltec V4). NSS/RST/DIO1/BUSY are in config.h
// since the sender shares them.
#define LORA_SCK           9
#define LORA_MISO         11
#define LORA_MOSI         10


// ════════════════════════════════════════════════════════════════════
//  PERIPHERAL OBJECTS
// ════════════════════════════════════════════════════════════════════

// SX1262 radio (NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool   loraReady = false;

// Set by the radio's interrupt when a packet finishes arriving. Kept as the
// only thing the ISR touches — all the real work happens in loop(), not here.
volatile bool packetFlag = false;
void IRAM_ATTR setPacketFlag() {
    packetFlag = true;
}

static uint32_t rxCount     = 0;
static uint32_t crcErrCount = 0;
static uint32_t otherErrCount = 0;

// Identity string for downstream host tooling — bump the version suffix if
// the JSON schema below ever changes in a way a host script needs to know.
#define DEVICE_ID  "GREENPOWER_RX_V1"

// esc_mode_code/esc_state_code → string — the LoRa packet carries 1-byte
// codes now instead of 8-byte ASCII strings (see config.h's own comment on
// PKT_ESC_MODE_*/PKT_ESC_STATE_*); decoded back to real strings here,
// receiver-side, for the pretty dump and the JSON line. Must match
// ../esc%20controller/throttle_controller.ino's modeName()/stateName().
static const char* escModeToStr(uint8_t code) {
    switch (code) {
        case PKT_ESC_MODE_ECO:    return "ECO";
        case PKT_ESC_MODE_SPORT:  return "SPORT";
        case PKT_ESC_MODE_NORMAL: return "NORMAL";
        default:                  return "---";
    }
}
static const char* escStateToStr(uint8_t code) {
    switch (code) {
        case PKT_ESC_STATE_IDLE:  return "IDLE";
        case PKT_ESC_STATE_REENG: return "REENG";
        case PKT_ESC_STATE_RAMP:  return "RAMP";
        case PKT_ESC_STATE_HOLD:  return "HOLD";
        default:                  return "---";
    }
}

// Answers "ID?" from the USB host without needing a full packet round-trip.
// Only ever called from loop(), not from the radio ISR.
static void pollIdentityRequest() {
    static char buf[16];
    static uint8_t idx = 0;

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            buf[idx] = '\0';
            if (idx > 0 && strcmp(buf, "ID?") == 0) {
                Serial.println(DEVICE_ID);
            }
            idx = 0;
        } else if (idx < sizeof(buf) - 1) {
            buf[idx++] = c;
        }
    }
}


// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);  // wait up to 3s for serial monitor
    Serial.println("\n[BOOT] Greenpower Receiver V1");
    Serial.println(DEVICE_ID);   // one-shot identity beacon for host tooling

    // SX1262 LoRa radio
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    int loraState = radio.begin(
        LORA_FREQ_MHZ,        // 915.0 MHz
        62.5,                 // bandwidth kHz — pushed down again from 125 for MAX range within the 200ms budget; MUST match the sender's copy exactly or packets won't decode — see this file's own header comment for the full history
        7,                    // spreading factor — SF7, the lowest value already proven safe on this exact hardware; MUST match the sender's copy exactly or packets won't decode (see config.h's own "must stay in sync" rule)
        5,                    // coding rate 4/5
        LORA_SYNC_WORD,       // 0xF3
        LORA_TX_POWER_DBM,    // unused for RX, kept for signature symmetry with sender
        LORA_PREAMBLE_SYMBOLS // preamble length — see its own comment in config.h; MUST match the sender's copy
    );
    if (loraState != RADIOLIB_ERR_NONE) {
        Serial.printf("[WARN] SX1262 init failed  code=%d\n", loraState);
    } else {
        radio.setDio2AsRfSwitch(true);   // required on Heltec V4
        radio.setPacketReceivedAction(setPacketFlag);

        // Implicit header mode — MUST match the sender's own implicitHeader()
        // call (same fixed length) exactly, or packets won't decode. See the
        // sender's radio.begin() block for the full reasoning; this side just
        // needs to agree, since implicit-header receivers have to already
        // know the payload length instead of reading it off an explicit
        // header the sender no longer sends.
        int hdrState = radio.implicitHeader(sizeof(telemetry_packet_t));
        if (hdrState != RADIOLIB_ERR_NONE) {
            Serial.printf("[WARN] implicitHeader() failed  code=%d — sender/receiver MUST agree on header mode or every packet will fail to decode\n", hdrState);
        }

        int rxState = radio.startReceive();
        if (rxState != RADIOLIB_ERR_NONE) {
            Serial.printf("[WARN] startReceive() failed  code=%d\n", rxState);
        } else {
            loraReady = true;
            Serial.printf("[OK]   SX1262  915 MHz  SF7  BW62.5  22dBm  implicit-hdr  preamble=%u  listening...\n", LORA_PREAMBLE_SYMBOLS);
        }
    }

    Serial.println("[RDY]  Waiting for packets\n");
}


// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════

void loop() {
    pollIdentityRequest();   // cheap, runs every loop regardless of packet timing

    if (!loraReady || !packetFlag) return;
    packetFlag = false;

    telemetry_packet_t pkt = {};
    int state = radio.readData((uint8_t*)&pkt, sizeof(pkt));

    if (state == RADIOLIB_ERR_NONE) {
        rxCount++;

        // ── Dump to USB serial ───────────────────────────────────────
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  Packet #%lu  RSSI:%.0f dBm  SNR:%.1f dB\n",
                      (unsigned long)rxCount, radio.getRSSI(), radio.getSNR());

        // Timestamp — pkt.epoch_time is a raw Unix-seconds int on the wire
        // (smallest possible over-the-air date/time representation, see
        // config.h); the human-readable string is built here, receiver-side,
        // entirely off the radio link, so it costs nothing in airtime.
        char tsBuf[24];
        if (pkt.epoch_time == 0) {
            snprintf(tsBuf, sizeof(tsBuf), "NO_RTC");
        } else {
            time_t rawTime = (time_t)pkt.epoch_time;
            struct tm tmInfo;
            gmtime_r(&rawTime, &tmInfo);
            strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", &tmInfo);
        }
        Serial.printf("  Timestamp : %s UTC\n", tsBuf);

        // Decoded back to human units from pkt's compressed fields — see
        // config.h's "Fixed-point compression" block. Done once here so the
        // pretty-print block and the JSON block below both read off the
        // same values instead of re-deriving them twice.
        float speedMph  = pkt.speed_mph_x10  / PKT_SCALE_SPEED;
        float tempF     = (pkt.temp_f_x10 == PKT_TEMP_NO_READING) ? NAN : (pkt.temp_f_x10 / PKT_SCALE_TEMP);
        float battVolt  = pkt.batt_volt_x100  / PKT_SCALE_VOLT;
        float motorVolt = pkt.motor_volt_x100 / PKT_SCALE_VOLT;
        float currentA  = pkt.current_a_x100  / PKT_SCALE_CURRENT;
        float pitchDeg  = pkt.pitch_deg_x100  / PKT_SCALE_ANGLE;
        float accelG    = pkt.accel_g_x1000   / PKT_SCALE_G;
        float lateralG  = pkt.lateral_g_x1000 / PKT_SCALE_G;
        float verticalG = pkt.vertical_g_x1000 / PKT_SCALE_G;
        float wheelRpm  = pkt.wheel_rpm_x10   / PKT_SCALE_WHEEL_RPM;
        float hdop = (pkt.hdop_x10 == PKT_HDOP_NO_FIX) ? 99.9f : (pkt.hdop_x10 / 10.0f);

        // Power
        Serial.printf("  Motor Volt: %.2f V\n",  motorVolt);
        Serial.printf("  Batt Volt : %.2f V\n",  battVolt);
        Serial.printf("  Current   : %.2f A\n",  currentA);

        // RPM
        Serial.printf("  Motor RPM : %u\n",      pkt.motor_rpm);
        Serial.printf("  Wheel RPM : %.1f\n",    wheelRpm);

        // Temperature
        Serial.printf("  Temp      : %.1f °F\n", tempF);

        // GPS
        Serial.printf("  GPS valid : %s\n",      (pkt.flags & PKT_FLAG_GPS_VALID) ? "YES" : "NO");
        Serial.printf("  Satellites: %u\n",       pkt.satellites);
        Serial.printf("  Speed     : %.1f mph\n", speedMph);
        Serial.printf("  Latitude  : %.6f\n",     pkt.latitude);
        Serial.printf("  Longitude : %.6f\n",     pkt.longitude);
        Serial.printf("  HDOP      : %.1f\n",     hdop);

        // IMU
        Serial.printf("  IMU valid : %s\n",      (pkt.flags & PKT_FLAG_IMU_VALID) ? "YES" : "NO");
        Serial.printf("  Pitch     : %.2f °\n",   pitchDeg);
        Serial.printf("  Accel     : %.3f g\n",   accelG);
        Serial.printf("  Lateral   : %.3f g\n",   lateralG);
        Serial.printf("  Vertical  : %.3f g\n",   verticalG);

        // ESC
        if (pkt.flags & PKT_FLAG_ESC_VALID) {
            Serial.printf("  ESC Mode  : %s\n",      escModeToStr(pkt.esc_mode_code));
            Serial.printf("  ESC State : %s\n",      escStateToStr(pkt.esc_state_code));
            Serial.printf("  Setpoint  : %u %%\n", pkt.esc_setpoint_pct);
            Serial.printf("  Live      : %u %%\n", pkt.esc_live_pct);
            Serial.printf("  Ramp      : %u %%\n", pkt.esc_ramp_pct);
        } else {
            Serial.println("  ESC       : waiting for data...");
        }

        Serial.println("──────────────────────────────────────────\n");

        // ── Machine-readable line for host tooling (receiver_agent) ───
        // Kept as a single line, prefixed so it's trivial to filter out of
        // the human dump above with a simple startsWith("JSON:") check.
        // Keys/units here are UNCHANGED from before the packet compression —
        // telemetry_web and receiver_agent both consume real human units
        // (mph, volts, °F, ...), so the wire-level fixed-point encoding is
        // entirely invisible past this point; no downstream change needed.
        char json[540];
        snprintf(json, sizeof(json),
            "JSON:{"
            "\"seq\":%lu,\"rssi\":%.0f,\"snr\":%.1f,\"flags\":%u,"
            "\"epoch_time\":%lu,\"timestamp\":\"%s\","
            "\"speed_mph\":%.1f,\"latitude\":%.6f,\"longitude\":%.6f,"
            "\"hdop\":%.1f,\"satellites\":%u,\"temp_f\":%.1f,"
            "\"batt_volt\":%.2f,\"motor_volt\":%.2f,\"current_a\":%.2f,"
            "\"pitch_deg\":%.2f,"
            "\"accel_g\":%.3f,\"lateral_g\":%.3f,\"vertical_g\":%.3f,"
            "\"motor_rpm\":%u,\"wheel_rpm\":%.1f,"
            "\"esc_valid\":%s,\"esc_mode\":\"%s\",\"esc_state\":\"%s\","
            "\"esc_setpoint_pct\":%u,\"esc_live_pct\":%u,\"esc_ramp_pct\":%u"
            "}",
            (unsigned long)rxCount, radio.getRSSI(), radio.getSNR(), pkt.flags,
            (unsigned long)pkt.epoch_time, tsBuf,
            speedMph, pkt.latitude, pkt.longitude,
            hdop, pkt.satellites, tempF,
            battVolt, motorVolt, currentA,
            pitchDeg,
            accelG, lateralG, verticalG,
            pkt.motor_rpm, wheelRpm,
            (pkt.flags & PKT_FLAG_ESC_VALID) ? "true" : "false",
            escModeToStr(pkt.esc_mode_code), escStateToStr(pkt.esc_state_code),
            pkt.esc_setpoint_pct, pkt.esc_live_pct, pkt.esc_ramp_pct
        );
        Serial.println(json);

    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        crcErrCount++;
        Serial.printf("[WARN] CRC mismatch, packet dropped  (total: %lu)\n", (unsigned long)crcErrCount);
    } else {
        otherErrCount++;
        Serial.printf("[WARN] readData() error %d  (total: %lu)\n", state, (unsigned long)otherErrCount);
    }

    // Always go back to listening, even after an error — otherwise the
    // radio stays in the finished-RX state and never hears anything again.
    radio.startReceive();
}
