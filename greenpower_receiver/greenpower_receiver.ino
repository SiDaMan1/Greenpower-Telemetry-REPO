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
//           Same RF settings as the sender (915 MHz, SF7, BW125, sync 0xF3)
//           — see config.h, which must stay in sync with the sender's copy.
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
        125.0,                // bandwidth kHz
        7,                    // spreading factor
        5,                    // coding rate 4/5
        LORA_SYNC_WORD,       // 0xF3
        LORA_TX_POWER_DBM,    // unused for RX, kept for signature symmetry with sender
        8                     // preamble length
    );
    if (loraState != RADIOLIB_ERR_NONE) {
        Serial.printf("[WARN] SX1262 init failed  code=%d\n", loraState);
    } else {
        radio.setDio2AsRfSwitch(true);   // required on Heltec V4
        radio.setPacketReceivedAction(setPacketFlag);

        int rxState = radio.startReceive();
        if (rxState != RADIOLIB_ERR_NONE) {
            Serial.printf("[WARN] startReceive() failed  code=%d\n", rxState);
        } else {
            loraReady = true;
            Serial.println("[OK]   SX1262  915 MHz  SF7  BW125  22dBm  listening...");
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

        // Power
        Serial.printf("  Motor Volt: %.2f V\n",  pkt.motor_volt);
        Serial.printf("  Batt Volt : %.2f V\n",  pkt.batt_volt);
        Serial.printf("  Current   : %.2f A\n",  pkt.current_a);

        // RPM
        Serial.printf("  Motor RPM : %.0f\n",    pkt.motor_rpm);
        Serial.printf("  Wheel RPM : %.0f\n",    pkt.wheel_rpm);

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

        // ── Machine-readable line for host tooling (receiver_agent) ───
        // Kept as a single line, prefixed so it's trivial to filter out of
        // the human dump above with a simple startsWith("JSON:") check.
        char json[400];
        snprintf(json, sizeof(json),
            "JSON:{"
            "\"seq\":%lu,\"rssi\":%.0f,\"snr\":%.1f,\"flags\":%u,"
            "\"speed_mph\":%.2f,\"latitude\":%.6f,\"longitude\":%.6f,"
            "\"hdop\":%.1f,\"satellites\":%u,\"temp_f\":%.1f,"
            "\"batt_volt\":%.2f,\"motor_volt\":%.2f,\"current_a\":%.2f,"
            "\"roll_deg\":%.2f,\"pitch_deg\":%.2f,\"yaw_deg\":%.2f,"
            "\"accel_g\":%.3f,\"lateral_g\":%.3f,\"vertical_g\":%.3f,"
            "\"motor_rpm\":%.0f,\"wheel_rpm\":%.0f"
            "}",
            (unsigned long)rxCount, radio.getRSSI(), radio.getSNR(), pkt.flags,
            pkt.speed_mph, pkt.latitude, pkt.longitude,
            pkt.hdop, pkt.satellites, pkt.temp_f,
            pkt.batt_volt, pkt.motor_volt, pkt.current_a,
            pkt.roll_deg, pkt.pitch_deg, pkt.yaw_deg,
            pkt.accel_g, pkt.lateral_g, pkt.vertical_g,
            pkt.motor_rpm, pkt.wheel_rpm
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
