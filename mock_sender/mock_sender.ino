// mock_sender.ino — ESP-NOW mock data sender for display_receiver
//
// HOW TO USE:
//   1. Flash display_receiver and open its Serial monitor to get its MAC address.
//   2. Paste that MAC into RECEIVER_MAC below.
//   3. Flash this sketch onto a second ESP32 and power it on.
//
// Packet format (CSV, matches display_receiver parsePacket):
//   speedMph,batV,rpm,amps,mode,state,setpointPct,livePct

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// ─── Target MAC ──────────────────────────────────────────────────────────────
// Replace with the MAC printed by display_receiver on boot.
uint8_t RECEIVER_MAC[6] = { 0x44, 0x1B, 0xF6, 0xCA, 0x38, 0xE4 };

// ─── Send interval ───────────────────────────────────────────────────────────
static const unsigned long SEND_MS = 22;   // ~45 Hz (22ms)

// ─── Demo simulation (mirrors demoTick in display_receiver) ──────────────────
struct MockData {
  float speedMph;
  float batV;
  float rpm;
  float amps;
  const char* mode;
  const char* state;
  float setpointPct;
  float livePct;
};

static MockData buildFrame(unsigned long ms) {
  MockData d;
  float t = (ms % 10000) / 10000.0f;

  if (t < 0.45f) {
    float p         = t / 0.45f;
    d.speedMph      = p * 26.0f;
    d.rpm           = 350.0f + p * 1450.0f;
    d.amps          = 8.0f  + p * 62.0f;
    d.setpointPct   = 80.0f;
    d.livePct       = p * 80.0f;
    d.state         = "RAMPING";
  } else if (t < 0.65f) {
    d.speedMph      = 26.0f;
    d.rpm           = 1700.0f;
    d.amps          = 36.0f;
    d.setpointPct   = 80.0f;
    d.livePct       = 80.0f;
    d.state         = "HOLD";
  } else if (t < 0.72f) {
    float p         = (t - 0.65f) / 0.07f;
    d.speedMph      = 26.0f + p * 2.0f;
    d.rpm           = 1700.0f + p * 500.0f;
    d.amps          = 60.0f  + p * 35.0f;
    d.setpointPct   = 100.0f;
    d.livePct       = 80.0f  + p * 20.0f;
    d.state         = "REENG";
  } else {
    float p         = (t - 0.72f) / 0.28f;
    d.speedMph      = 28.0f * (1.0f - p);
    d.rpm           = 2200.0f * (1.0f - p) + 200.0f;
    d.amps          = 95.0f  * (1.0f - p);
    d.setpointPct   = 100.0f * (1.0f - p);
    d.livePct       = 100.0f * (1.0f - p);
    d.state         = "IDLE";
  }

  // Slowly drain battery, reset when it hits dead band
  d.batV = 25.4f - (ms / 3600000.0f) * 2.0f;
  if (d.batV < 20.0f) d.batV = 25.4f;

  const char* modes[] = { "NORMAL", "SPORT", "ECO" };
  d.mode = modes[(ms / 12000) % 3];

  return d;
}

// ─── ESP-NOW send callback ────────────────────────────────────────────────────
void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  // optional: Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  delay(100);
  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed — halting");
    while (true) delay(1000);
  }

  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("add_peer failed — check MAC");
  }

  Serial.println("Mock sender ready");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
static unsigned long lastSend = 0;

void loop() {
  unsigned long now = millis();
  if (now - lastSend < SEND_MS) return;
  lastSend = now;

  MockData d = buildFrame(now);

  char pkt[128];
  snprintf(pkt, sizeof(pkt),
    "%.2f,%.2f,%.1f,%.1f,%.7s,%.7s,%.1f,%.1f",
    d.speedMph, d.batV, d.rpm, d.amps,
    d.mode, d.state,
    d.setpointPct, d.livePct);

  esp_now_send(RECEIVER_MAC, (uint8_t*)pkt, strlen(pkt));

  Serial.println(pkt);
}
