#include <Adafruit_GFX.h>
#include "display_bsp.h"
#include "dash_data.h"
#include <esp_now.h>
#include <WiFi.h>

// Waveshare RLCD compatible fonts
#include "Fonts/BigShoulders_18pt_SemiBold9pt7b.h"
#include "Fonts/BigShoulders_18pt_SemiBold12pt7b.h"
#include "Fonts/BigShoulders_18pt_SemiBold14pt7b.h"
#include <math.h>

// Font Aliases
#define FONT_MAIN &BigShoulders_18pt_SemiBold9pt7b
#define FONT_BIG  &BigShoulders_18pt_SemiBold12pt7b

#define DASH_VERSION "V1"

static const int W = 400, H = 300;

DisplayPort display(12, 11, 5, 40, 41, W, H);
GFXcanvas1  canvas(W, H);

// --- ESP-NOW ---
static DashData liveData;
static volatile unsigned long lastPacket = 0;
static volatile bool hasPacket = false;

void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  char buf[96];
  int n = min(len, (int)sizeof(buf) - 1);
  memcpy(buf, data, n);
  buf[n] = '\0';
  DashData d;
  if (sscanf(buf, "%f,%f,%f,%f,%7[^,],%7[^,],%f,%f",
      &d.speedMph, &d.batV, &d.rpm, &d.amps,
      d.mode, d.state, &d.setpointPct, &d.livePct) == 8) {
    liveData   = d;
    lastPacket = millis();
    hasPacket  = true;
  }
}

// --- HARDWARE SENSING ---
static const int   BAT_ADC_PIN  = 4;     // GPIO4 = ADC1 ch3
static const int   BATT_EN_PIN  = 21;    // enables voltage divider
static const int   BTN_PIN      = 0;     // BOOT button (active LOW) — change if using a different pin

static bool demoMode = false;            // start in live mode; button toggles to demo

// Formula from working Waveshare example on this board:
//   v_bat = (raw/4095) * 3.3 * 3.0 * 1.079
// Divider is ~3:1, empirical cal factor 1.079
static float g_scrSmooth = -1.f;  // shared EMA state for V and %

// Call once per frame — reads ADC, updates smoother, returns smoothed voltage.
// Asymmetric EMA: tracks voltage drops at normal speed (real discharge),
// but resists upward jumps (charging artifact) with a very slow rise rate.
float scrBatUpdate() {
  int raw = analogRead(BAT_ADC_PIN);
  float v = (raw / 4095.0f) * 3.3f * 3.0f * 1.079f;
  if (v < 0.5f) return 0.f;                    // no cell
  if (g_scrSmooth < 0.f) g_scrSmooth = v;      // seed on first call
  if (v < g_scrSmooth)
    g_scrSmooth = g_scrSmooth * 0.997f  + v * 0.003f;   // drop: ~7s time constant
  else
    g_scrSmooth = g_scrSmooth * 0.9998f + v * 0.0002f;  // rise: ~110s time constant
  return g_scrSmooth;
}

int scrBatPct(float sv) {
  // 18650 discharge curve: flat mid-range around 3.6-3.7V, steep at ends
  static const float curvV[] = {3.00f, 3.20f, 3.40f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f, 4.25f};
  static const float curvP[] = {  0.f,   5.f,  12.f,  30.f,  50.f,  65.f,  78.f,  88.f,  95.f,  99.f, 100.f};
  static const int   N = 11;
  if (sv <= 0.f || sv <= curvV[0]) return 0;
  if (sv >= curvV[N-1]) return 100;
  for (int i = 1; i < N; i++) {
    if (sv <= curvV[i]) {
      float frac = (sv - curvV[i-1]) / (curvV[i] - curvV[i-1]);
      return (int)(curvP[i-1] + frac * (curvP[i] - curvP[i-1]));
    }
  }
  return 100;
}

int batPct(float v) {
  // 2x 12V lead-acid discharge curve (14V–25.4V)
  // Steep %/V above 23V (slow voltage drop = lots of energy per volt)
  // Shallow %/V below 23V (fast voltage drop = little energy per volt)
  // → each % roughly represents the same amount of remaining run time
  static const float curvV[] = {14.0f, 18.0f, 20.0f, 21.0f, 21.5f, 22.0f, 22.5f, 23.0f, 23.5f, 24.0f, 24.5f, 25.0f, 25.4f};
  static const float curvP[] = {  0.f,   2.f,   5.f,  10.f,  15.f,  22.f,  30.f,  40.f,  52.f,  65.f,  78.f,  90.f, 100.f};
  static const int N = 13;
  if (v <= curvV[0])   return 0;
  if (v >= curvV[N-1]) return 100;
  for (int i = 1; i < N; i++) {
    if (v <= curvV[i]) {
      float frac = (v - curvV[i-1]) / (curvV[i] - curvV[i-1]);
      return (int)(curvP[i-1] + frac * (curvP[i] - curvP[i-1]));
    }
  }
  return 100;
}

// --- DASHBOARD DATA GENERATOR (DEMO) ---

DashData demoTick(unsigned long ms) {
  DashData d;
  float t = (ms % 8000) / 8000.f;
  // smooth continuous sine: 0 → 1 → 0 with no plateau
  float p = (1.f - cosf(t * 2.f * (float)M_PI)) * 0.5f;

  d.speedMph    = constrain(p * 26.f,          0.f, 30.f);
  d.rpm         = constrain(350.f + p * 1350.f, 0.f, 2500.f);
  d.amps        = constrain(8.f   + p * 62.f,  0.f, 100.f);
  d.setpointPct = constrain(p * 80.f,          0.f, 100.f);
  d.livePct     = constrain(p * 80.f,          0.f, 100.f);

  if      (p < 0.15f) strcpy(d.state, "IDLE");
  else if (t < 0.5f)  strcpy(d.state, "RAMPING");
  else                strcpy(d.state, "HOLD");

  // Battery drains 25.4V → 20.0V over 90 min, then wraps
  d.batV = constrain(25.4f - (ms % 5400000UL) / 5400000.f * 5.4f, 20.0f, 25.6f);

  const char* modes[] = {"NORMAL", "SPORT", "ECO"};
  strcpy(d.mode, modes[(ms / 12000) % 3]);
  return d;
}

// --- RLCD RENDERING FUNCTIONS ---

void drawInvertedBarText(int x, int y, int w, int h, int fillW, bool isCheckerboard, const char* label) {
  canvas.fillRect(x + 1, y + 1, w - 2, h - 2, 0);
  canvas.setFont(FONT_MAIN);
  canvas.setTextColor(1);
  int16_t x1, y1; uint16_t tw, th;
  canvas.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  int tx = x + (w - (int)tw) / 2;
  int ty = y + (h + (int)th) / 2;
  canvas.setCursor(tx, ty);
  canvas.print(label);

  for (int iy = y + 1; iy < y + h - 1; iy++) {
    for (int ix = x + 1; ix < x + 1 + fillW; ix++) {
      bool isText = canvas.getPixel(ix, iy);
      if (isCheckerboard) {
        canvas.drawPixel(ix, iy, isText ? 0 : (((ix + iy) % 2 == 0) ? 1 : 0));
      } else {
        canvas.drawPixel(ix, iy, !isText);
      }
    }
  }
}

void battBar(int x, int y, int w, int h, int pct, const char* label) {
  canvas.drawRect(x, y, w, h, 1);
  canvas.fillRect(x + w, y + h / 2 - 2, 3, 4, 1);
  int f = constrain((int)((w - 2) * pct / 100), 0, w - 2);
  drawInvertedBarText(x, y, w, h, f, false, label);
}

void ampBar(int x, int y, int w, int h, float amps, float maxA, const char* label) {
  canvas.drawRect(x, y, w, h, 1);
  int f = constrain((int)((w - 2) * (amps / maxA)), 0, w - 2);
  drawInvertedBarText(x, y, w, h, f, (amps > maxA * 0.8f), label);
  canvas.drawFastVLine(x + 1 + (int)((w - 2) * 0.8f), y, h, 1);
}

float gaugeAng(float val, float lo, float hi) {
  float n = constrain((val - lo) / (hi - lo), 0, 1);
  return (225.0f - n * 270.0f) * (float)M_PI / 180.0f;
}

void arc(int cx, int cy, int r, float dLo, float dHi, int t) {
  for (float d = dLo; d <= dHi + 0.01f; d += 0.7f) {
    float a = d * (float)M_PI / 180.0f;
    int x = cx + (int)(r * cosf(a)), y = cy - (int)(r * sinf(a));
    canvas.drawPixel(x, y, 1);
    if (t >= 2) { canvas.drawPixel(x + 1, y, 1); canvas.drawPixel(x, y + 1, 1); }
  }
}

void tick(int cx, int cy, int ro, int ri, float deg) {
  float a = deg * (float)M_PI / 180.0f;
  canvas.drawLine(cx + (int)(ro * cosf(a)), cy - (int)(ro * sinf(a)),
                  cx + (int)(ri * cosf(a)), cy - (int)(ri * sinf(a)), 1);
}


void ct(const GFXfont* f, const char* s, int px, int py) {
  canvas.setFont(f);
  canvas.setTextColor(1);
  int16_t x1, y1; uint16_t w, h;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor(px - (int)w / 2, py + (int)h / 2);
  canvas.print(s);
}

void ctClear(const GFXfont* f, const char* s, int px, int py, int pad = 3) {
  canvas.setFont(f);
  int16_t x1, y1; uint16_t tw, th;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  int cx_ = px - (int)tw / 2, cy_ = py + (int)th / 2;
  canvas.fillRect(cx_ + x1 - pad, cy_ + y1 - pad, (int)tw + pad * 2, (int)th + pad * 2, 0);
  canvas.setTextColor(1);
  canvas.setCursor(cx_, cy_);
  canvas.print(s);
}

void badge(int x, int y, int w, int h, const char* s) {
  canvas.fillRect(x, y, w, h, 1);
  canvas.setFont(FONT_MAIN);
  canvas.setTextColor(0);
  int16_t x1, y1; uint16_t bw, bh;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &bw, &bh);
  canvas.setCursor(x + (w - (int)bw) / 2, y + (h + (int)bh) / 2);
  canvas.print(s);
  canvas.setTextColor(1);
}

void drawGauge(int cx, int cy, int r, float val, float lo, float hi,
               int tickStep, int labelStep,
               const char* bigNum, const char* unit,
               const GFXfont* numFont, int numDY,
               bool showLabels = false, int unitGap = 22) {
  arc(cx, cy, r, -45.f, 225.f, 2);
  arc(cx, cy, r - 4, -45.f, 225.f, 1);
  if (val > lo + 0.5f) arc(cx, cy, r - 9, 225.f - ((val - lo) / (hi - lo)) * 270.f, 225.f, 3);

  for (float v = lo; v <= hi + 0.1f; v += tickStep) {
    float deg = 225.f - ((v - lo) / (hi - lo)) * 270.f;
    bool maj = (((int)v % labelStep) == 0);
    tick(cx, cy, r, r - 10, deg);
    if (showLabels && maj && deg >= -46.f) {
      char buf[8]; snprintf(buf, sizeof(buf), hi >= 1000 ? "%dk" : "%d", hi >= 1000 ? (int)v / 1000 : (int)v);
      float a = deg * (float)M_PI / 180.f;
      ct(FONT_MAIN, buf, cx + (int)((r - 24) * cosf(a)), cy - (int)((r - 24) * sinf(a)));
    }
  }

  float ang = gaugeAng(val, lo, hi);
  canvas.drawLine(cx, cy, cx + (int)((r - 8) * cosf(ang)), cy - (int)((r - 8) * sinf(ang)), 1);
  canvas.fillCircle(cx, cy, 6, 1);
  canvas.fillCircle(cx, cy, 3, 0);
  ctClear(numFont,   bigNum, cx, cy + numDY,          3);
  ctClear(FONT_MAIN, unit,   cx, cy + numDY + unitGap, 2);
}

void drawMiniGauge(int cx, int cy, int r, float val, float lo, float hi, const char* numStr, const char* label) {
  arc(cx, cy, r, -45.f, 225.f, 2);
  if (val > lo + 0.5f) arc(cx, cy, r - 5, 225.f - ((val - lo) / (hi - lo)) * 270.f, 225.f, 2);
  float ang = gaugeAng(val, lo, hi);
  canvas.drawLine(cx, cy, cx + (int)((r - 3) * cosf(ang)), cy - (int)((r - 3) * sinf(ang)), 1);
  canvas.fillCircle(cx, cy, 3, 1);
  ctClear(FONT_MAIN, numStr, cx, cy + 14, 2);
  ct(FONT_MAIN, label, cx, cy + r + 8);
}

// --- DASHBOARD UI ASSEMBLY ---

void drawDash(const DashData& d, bool demo = false) {
  canvas.fillScreen(0);
  badge(3, 3, 90, 24, d.mode);
  badge(97, 3, 108, 24, d.state);

  // Top Right - Internal Battery (always reads real GPIO4 ADC)
  {
    char  s[32];
    float sv = scrBatUpdate();
    int   sp = scrBatPct(sv);
    snprintf(s, sizeof(s), demo ? "SCR %.1fV %d%% DEMO" : "SCR %.1fV %d%%", sv, sp);
    battBar(211, 4, 182, 22, sp, s);
  }

  canvas.drawFastHLine(0, 30, W, 1);
  canvas.drawFastVLine(120, 31, 189, 1);
  canvas.drawFastVLine(300, 31, 189, 1);

  ct(FONT_MAIN, "MOTOR RPM", 60,  201);
  ct(FONT_MAIN, "SPEED",    210,  201);

  char b1[16], b2[16], b3[16], b4[16];
  snprintf(b1, sizeof(b1), "%.0f", d.rpm);
  drawGauge(60, 127, 50, d.rpm, 0, 2500, 250, 1000, b1, "RPM", FONT_MAIN, 26, false, 18);

  snprintf(b2, sizeof(b2), "%.1f", d.speedMph);
  drawGauge(210, 124, 76, d.speedMph, 0, 30, 5, 10, b2, "MPH", &BigShoulders_18pt_SemiBold14pt7b, 28, true);

  snprintf(b3, sizeof(b3), "%.0f%%", d.setpointPct);
  drawMiniGauge(350, 78, 30, d.setpointPct, 0, 100, b3, "SET");

  snprintf(b4, sizeof(b4), "%.0f%%", d.livePct);
  drawMiniGauge(350, 163, 30, d.livePct, 0, 100, b4, "LIVE");

  canvas.drawFastHLine(0, 220, W, 1);
  char al[32]; snprintf(al, sizeof(al), "CURRENT  %.0fA", d.amps);
  ampBar(5, 225, 389, 28, d.amps, 100.f, al);

  canvas.drawFastHLine(0, 259, W, 1);
  int bp = batPct(d.batV);
  char bl[32]; snprintf(bl, sizeof(bl), "BATT %.1fV %d%%", d.batV, bp);
  battBar(5, 263, 389, 30, bp, bl);
}

void drawSplash() {
  canvas.fillScreen(1);   // black background
  canvas.setFont(&BigShoulders_18pt_SemiBold14pt7b);
  canvas.setTextColor(0); // white text

  const char* lines[] = {"GP RACING DASHBOARD", DASH_VERSION};
  const int   nLines = 2;
  const int   gap    = 12;

  int16_t x1, y1; uint16_t tw, th;
  // Use a capital letter to get a consistent line height for the font
  canvas.getTextBounds("A", 0, 0, &x1, &y1, &tw, &th);
  int lh      = (int)th;
  int totalH  = nLines * lh + (nLines - 1) * gap;
  int blockTop = H / 2 - totalH / 2;

  for (int i = 0; i < nLines; i++) {
    canvas.getTextBounds(lines[i], 0, 0, &x1, &y1, &tw, &th);
    int lineTop = blockTop + i * (lh + gap);
    canvas.setCursor(W/2 - (int)tw/2 - x1, lineTop - y1);
    canvas.print(lines[i]);
  }
}

void drawWaiting() {
  canvas.fillScreen(1);          // black background
  canvas.setFont(&BigShoulders_18pt_SemiBold14pt7b);
  canvas.setTextColor(0);        // white text
  const char* msg = "WAITING FOR DATA";
  int16_t x1, y1; uint16_t tw, th;
  canvas.getTextBounds(msg, 0, 0, &x1, &y1, &tw, &th);
  canvas.setCursor(W/2 - (int)tw/2 - x1, H/2 + (int)th/2);
  canvas.print(msg);
}

void pushCanvas() {
  uint8_t* buf = canvas.getBuffer();
  const int bpr = (W + 7) / 8;
  display.RLCD_ColorClear(ColorWhite);
  for (int y = 0; y < H; y++) {
    uint8_t* row = buf + y * bpr;
    for (int b = 0; b < bpr; b++) {
      uint8_t v = row[b];
      for (int bit = 0; bit < 8; bit++) {
        int x = b*8 + bit; if (x >= W) break;
        if (v & (0x80 >> bit)) display.RLCD_SetPixel(x, y, ColorBlack);
      }
    }
  }
  display.RLCD_Display();
}

void setup() {
  Serial.begin(115200);

  pinMode(BATT_EN_PIN, OUTPUT);
  digitalWrite(BATT_EN_PIN, HIGH);
  pinMode(BTN_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  delay(10);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onReceive);
    Serial.println("ESP-NOW ready");
  } else {
    Serial.println("ESP-NOW init failed");
  }

  display.RLCD_Init();
  display.RLCD_ColorClear(ColorWhite);

  drawSplash();
  pushCanvas();
  delay(2500);
}

void loop() {
  // Button debounce — toggles demoMode on each press
  static bool lastBtn = HIGH;
  static unsigned long btnTime = 0;
  bool btn = digitalRead(BTN_PIN);
  if (btn == LOW && lastBtn == HIGH && millis() - btnTime > 200) {
    demoMode = !demoMode;
    btnTime = millis();
  }
  lastBtn = btn;

  bool waiting = (!demoMode && (!hasPacket || millis() - lastPacket > 3000));

  // Waiting screen: draw once on transition, then idle — no background processing
  static bool wasWaiting = false;
  if (waiting) {
    if (!wasWaiting) { drawWaiting(); pushCanvas(); }
    wasWaiting = true;
    return;
  }
  wasWaiting = false;

  static unsigned long last = 0;
  if (millis() - last < 22) return;  // ~45 fps
  last = millis();

  if (demoMode) {
    drawDash(demoTick(last), true);
  } else {
    drawDash(liveData, false);
  }
  pushCanvas();
}
