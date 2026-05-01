#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "display_bsp.h"
#include <esp_now.h>
#include <WiFi.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <math.h>

static const int W = 400;
static const int H = 300;

// ── Screen battery (18650) ────────────────────────────────────────────────────
// Verify SCR_BATT_PIN from the Waveshare RLCD 4.2" dev board schematic.
// GPIO1 is the most common sense pin on this module (1:2 divider → Vadc = Vbat/2).
static const int   SCR_BATT_PIN = 1;
static const float SCR_BATT_DIV = 2.0f;

// ── Charging indicator ────────────────────────────────────────────────────────
// Set to the TP4056 CHRG GPIO (active LOW when charging). -1 = disabled.
static const int CHRG_PIN = -1;

DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1  canvas(W, H);

// ═══════════════════════════════════════════════════════════════════════
//  LAYOUT — all boundaries verified, nothing overlaps
//
//  ROW 0  y=0..29    HEADER
//    Mode badge   x=3,   w=90,  h=24, y=3
//    State badge  x=97,  w=108, h=24, y=3
//    SCR bat bar  x=210, w=182, h=17, y=6
//
//  ROW 1  y=30..219  GAUGES
//    x=0..120   RPM small    CX=60  CY=132 R=52
//    x=120..300 Speed large  CX=210 CY=132 R=78
//    x=300..400 SET mini     CX=350 CY=82  R=32
//               LIVE mini    CX=350 CY=169 R=32
//
//  ROW 2a y=220..258 AMPS bar (x=0..202) | RAMP bar (x=203..399)
//  ROW 2b y=259..299 Vehicle battery bar (full width)
// ═══════════════════════════════════════════════════════════════════════

// ─── Data ─────────────────────────────────────────────────────────────────────
struct DashData {
  float speedMph    = 0;
  float batV        = 24.6f;
  float rpm         = 0;
  float amps        = 0;
  char  mode[8]     = "NORMAL";
  char  state[8]    = "IDLE";
  float setpointPct = 0;
  float livePct     = 0;
  float rampPct     = 0;
};

// ─── ESP-NOW ──────────────────────────────────────────────────────────────────
volatile bool hasNew = false;
char          rxBuf[128];
portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;

void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 1 || len > 127) return;
  portENTER_CRITICAL_ISR(&mux);
  memcpy(rxBuf, data, len);
  rxBuf[len] = '\0';
  hasNew = true;
  portEXIT_CRITICAL_ISR(&mux);
}

bool parsePacket(const char* src, DashData& d) {
  char buf[128]; strncpy(buf, src, 127); buf[127] = '\0';
  char* t;
  t = strtok(buf,  ","); if (!t) return false; d.speedMph    = atof(t);
  t = strtok(NULL, ","); if (!t) return false; d.batV        = atof(t);
  t = strtok(NULL, ","); if (!t) return false; d.rpm         = atof(t);
  t = strtok(NULL, ","); if (!t) return false; d.amps        = atof(t);
  t = strtok(NULL, ","); if (!t) return false; strncpy(d.mode,  t, 7); d.mode[7]  = '\0';
  t = strtok(NULL, ","); if (!t) return false; strncpy(d.state, t, 7); d.state[7] = '\0';
  t = strtok(NULL, ","); if (!t) return false; d.setpointPct = atof(t);
  t = strtok(NULL, ","); if (!t) return false; d.livePct     = atof(t);
  t = strtok(NULL, ","); if (!t) return false; d.rampPct     = atof(t);
  return true;
}

// ─── Battery helpers ──────────────────────────────────────────────────────────
int batPct(float v) {                          // vehicle: 20V=0%, 25.6V=100%
  int p = (int)((v - 20.0f) / 5.6f * 100.0f);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

float scrBatV() {
  return analogRead(SCR_BATT_PIN) / 4095.0f * 3.3f * SCR_BATT_DIV;
}

int scrBatPct() {                              // 18650: 3.0V=0%, 4.2V=100%
  int p = (int)((scrBatV() - 3.0f) / 1.2f * 100.0f);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

bool isCharging() {
  if (CHRG_PIN < 0) return false;
  return digitalRead(CHRG_PIN) == LOW;         // TP4056 CHRG is active LOW
}

// ─── Drawing primitives ───────────────────────────────────────────────────────
float gaugeAng(float val, float lo, float hi) {
  float n = (val - lo) / (hi - lo);
  if (n < 0) n = 0; if (n > 1) n = 1;
  return (225.0f - n * 270.0f) * (float)M_PI / 180.0f;
}

void arc(int cx, int cy, int r, float dLo, float dHi, int t) {
  for (float d = dLo; d <= dHi + 0.01f; d += 0.7f) {
    float a = d * (float)M_PI / 180.0f;
    int x = cx + (int)(r * cosf(a));
    int y = cy - (int)(r * sinf(a));
    canvas.drawPixel(x, y, 1);
    if (t >= 2) { canvas.drawPixel(x+1,y,1); canvas.drawPixel(x,y+1,1); }
    if (t >= 3) { canvas.drawPixel(x-1,y,1); canvas.drawPixel(x,y-1,1); }
  }
}

void tick(int cx, int cy, int ro, int ri, float deg) {
  float a = deg * (float)M_PI / 180.0f;
  canvas.drawLine(cx+(int)(ro*cosf(a)), cy-(int)(ro*sinf(a)),
                  cx+(int)(ri*cosf(a)), cy-(int)(ri*sinf(a)), 1);
}

// Centre text on pixel (px, py)
void ct(const GFXfont* f, const char* s, int px, int py) {
  canvas.setFont(f);
  canvas.setTextColor(1);
  int16_t x1, y1; uint16_t w, h;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor(px - (int)w/2, py + (int)h/2);
  canvas.print(s);
}

// Inverted filled badge
void badge(int x, int y, int w, int h, const char* s) {
  canvas.fillRect(x, y, w, h, 1);
  canvas.setFont(&FreeMonoBold12pt7b);
  canvas.setTextColor(0);
  int16_t x1, y1; uint16_t bw, bh;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &bw, &bh);
  canvas.setCursor(x + (w-(int)bw)/2, y + (h+(int)bh)/2);
  canvas.print(s);
  canvas.setTextColor(1);
}

// Horizontal battery bar with fill, nub, and centred label
void battBar(int x, int y, int w, int h, int pct, const char* label) {
  canvas.drawRect(x, y, w, h, 1);
  canvas.fillRect(x+w, y+h/2-2, 3, 4, 1);           // nub
  int f = (int)((w-2) * pct / 100);
  if (f > 0) canvas.fillRect(x+1, y+1, f, h-2, 1);  // fill
  canvas.setFont(&FreeMono9pt7b);
  canvas.setTextColor(pct > 50 ? 0 : 1);
  int16_t x1_, y1_; uint16_t tw, th;
  canvas.getTextBounds(label, 0, 0, &x1_, &y1_, &tw, &th);
  canvas.setCursor(x + w/2 - (int)tw/2, y + h - 3);
  canvas.print(label);
  canvas.setTextColor(1);
}

// Horizontal progress bar (no nub, no text)
void hbar(int x, int y, int w, int h, float pct, bool hatch = false) {
  canvas.drawRect(x, y, w, h, 1);
  int f = (int)((w-2) * (pct > 100.f ? 1.f : pct / 100.f));
  if (f > 0) {
    if (hatch) {
      for (int fy = y+1; fy < y+h-1; fy++)
        for (int fx = x+1; fx < x+1+f; fx++)
          if ((fx+fy) % 2 == 0) canvas.drawPixel(fx, fy, 1);
    } else {
      canvas.fillRect(x+1, y+1, f, h-2, 1);
    }
  }
}

// ─── Main gauge (speed / RPM) ─────────────────────────────────────────────────
// numFont / numDY let the caller choose font size and vertical offset
void drawGauge(int cx, int cy, int r,
               float val, float lo, float hi,
               const char* bigNum, const char* unit,
               int dangerLo, int dangerHi, int effAt,
               int tickStep, int labelStep,
               const GFXfont* numFont, int numDY) {

  arc(cx, cy, r,   -45.f, 225.f, 2);
  arc(cx, cy, r-4, -45.f, 225.f, 1);

  // Danger zone hatch
  if (dangerLo > 0) {
    float dA = 225.f - ((float)dangerLo/hi)*270.f;
    float dB = 225.f - ((float)dangerHi/hi)*270.f;
    for (float dg = dB; dg <= dA; dg += 0.9f) {
      float a = dg*(float)M_PI/180.f;
      for (int ri = r-9; ri <= r+2; ri += 2)
        canvas.drawPixel(cx+(int)(ri*cosf(a)), cy-(int)(ri*sinf(a)), 1);
    }
  }

  // Progress fill
  if (val > lo + 0.5f)
    arc(cx, cy, r-9, 225.f - (val/hi)*270.f, 225.f, 3);

  // Efficiency marker
  if (effAt > 0) {
    float ed = 225.f - ((float)effAt/hi)*270.f;
    tick(cx, cy, r+4, r-14, ed);
    tick(cx, cy, r+4, r-14, ed+2.f);
  }

  // Ticks + labels (skip end-stop label at -45° to prevent overlap with number)
  for (float v = lo; v <= hi + 0.1f; v += tickStep) {
    float deg = 225.f - ((v-lo)/(hi-lo))*270.f;
    bool  maj = (((int)v % labelStep) == 0);
    tick(cx, cy, r, maj ? r-15 : r-7, deg);
    if (maj && deg > -40.f) {
      char buf[8];
      if (hi >= 1000) {
        if ((int)v == 0) snprintf(buf, sizeof(buf), "0");
        else             snprintf(buf, sizeof(buf), "%dk", (int)v/1000);
      } else {
        snprintf(buf, sizeof(buf), "%d", (int)v);
      }
      float a = deg*(float)M_PI/180.f;
      ct(&FreeMono9pt7b, buf, cx+(int)((r-26)*cosf(a)), cy-(int)((r-26)*sinf(a)));
    }
  }

  // Needle
  float ang = gaugeAng(val, lo, hi);
  int nx = cx+(int)((r-8)*cosf(ang)), ny = cy-(int)((r-8)*sinf(ang));
  int tx = cx-(int)(12*cosf(ang)),    ty = cy+(int)(12*sinf(ang));
  for (int dd = -2; dd <= 2; dd++) {
    int ox=(int)(dd*sinf(ang)), oy=(int)(dd*cosf(ang));
    canvas.drawLine(tx+ox, ty+oy, nx+ox, ny+oy, 1);
  }
  canvas.fillCircle(cx, cy, 6, 1);
  canvas.fillCircle(cx, cy, 3, 0);

  ct(numFont,        bigNum, cx, cy + numDY);
  ct(&FreeMono9pt7b, unit,   cx, cy + numDY + 20);
}

// ─── Mini gauge (SET / LIVE) ──────────────────────────────────────────────────
// Simple arc + fill + needle + centre number + label below gap
void drawMiniGauge(int cx, int cy, int r,
                   float val, float lo, float hi,
                   const char* numStr, const char* label) {
  arc(cx, cy, r, -45.f, 225.f, 2);

  // Fill arc
  if (val > lo + 0.5f)
    arc(cx, cy, r-6, 225.f - ((val-lo)/(hi-lo))*270.f, 225.f, 2);

  // Needle
  float ang = gaugeAng(val, lo, hi);
  canvas.drawLine(cx, cy,
                  cx+(int)((r-3)*cosf(ang)), cy-(int)((r-3)*sinf(ang)), 1);
  canvas.fillCircle(cx, cy, 3, 1);
  canvas.fillCircle(cx, cy, 1, 0);

  // Number inside arc (10px below centre — sits in the open gap)
  ct(&FreeMonoBold12pt7b, numStr, cx, cy+10);

  // Label below gap
  ct(&FreeMono9pt7b, label, cx, cy + r + 4);
}

// ─── Full dashboard ───────────────────────────────────────────────────────────
void drawDash(const DashData& d, bool demo) {
  canvas.fillScreen(0);

  // ══ ROW 0: HEADER  y=0..29 ══════════════════════════════════════════════════
  badge(3,  3,  90, 24, d.mode);
  badge(97, 3, 108, 24, d.state);

  // Screen battery bar (x=210 y=6 w=182 h=17)
  {
    int   sp  = scrBatPct();
    float sv  = scrBatV();
    bool  chg = isCharging();
    char  s[28];
    if      (demo && chg) snprintf(s, sizeof(s), "SCR %.1fV CHG DM", sv);
    else if (demo)        snprintf(s, sizeof(s), "SCR %.1fV %d%% DM", sv, sp);
    else if (chg)         snprintf(s, sizeof(s), "SCR %.1fV CHG",     sv);
    else                  snprintf(s, sizeof(s), "SCR %.1fV %d%%",    sv, sp);
    battBar(210, 6, 182, 17, sp, s);
  }

  canvas.drawFastHLine(0, 30, W, 1);

  // ══ ROW 1: GAUGES  y=30..219 ════════════════════════════════════════════════
  canvas.drawFastVLine(120, 30, 190, 1);   // RPM | Speed
  canvas.drawFastVLine(300, 30, 190, 1);   // Speed | Mini gauges

  // Section labels
  ct(&FreeMono9pt7b, "MOTOR RPM", 60,  40);
  ct(&FreeMono9pt7b, "SPEED",    210,  40);
  ct(&FreeMono9pt7b, "THROTTLE", 350,  40);

  // RPM — small gauge left (CX=60 CY=132 R=52)
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.0f", d.rpm);
    drawGauge(60, 132, 52,
              d.rpm, 0, 2500, buf, "RPM",
              2200, 2500, 1700, 250, 1000,
              &FreeMonoBold12pt7b, 12);
  }

  // Speed — large gauge centre (CX=210 CY=132 R=78)
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", d.speedMph);
    drawGauge(210, 132, 78,
              d.speedMph, 0, 30, buf, "MPH",
              0, 0, 0, 5, 10,
              &FreeMonoBold18pt7b, 22);
  }

  // SET mini gauge (CX=350 CY=82 R=32)
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.0f%%", d.setpointPct);
    drawMiniGauge(350, 82, 32, d.setpointPct, 0, 100, buf, "SET");
  }

  // LIVE mini gauge (CX=350 CY=169 R=32)
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.0f%%", d.livePct);
    drawMiniGauge(350, 169, 32, d.livePct, 0, 100, buf, "LIVE");
  }

  canvas.drawFastHLine(0, 220, W, 1);

  // ══ ROW 2a: AMPS + RAMP  y=220..258 ═════════════════════════════════════════
  canvas.drawFastVLine(203, 220, 39, 1);   // Amps | Ramp

  // AMPS (x=0..202)
  {
    char lbl[16]; snprintf(lbl, sizeof(lbl), "AMPS  %.0fA", d.amps);
    canvas.setFont(&FreeMono9pt7b);
    canvas.setTextColor(1);
    canvas.setCursor(5, 229);
    canvas.print(lbl);

    // Bar x=5 y=234 w=192 h=14
    const int AX=5, AY=234, AW=192, AH=14;
    int warnX = AX + 1 + (int)((AW-2) * 0.8f);
    hbar(AX, AY, AW, AH, d.amps, d.amps > 80.f);
    canvas.drawFastVLine(warnX, AY, AH, 1);  // 80A warning line

    // Scale labels
    canvas.setFont(&FreeMono9pt7b); canvas.setTextColor(1);
    canvas.setCursor(AX,        AY+AH+8); canvas.print("0");
    canvas.setCursor(warnX-7,   AY+AH+8); canvas.print("80");
    canvas.setCursor(AX+AW-14,  AY+AH+8); canvas.print("^");
  }

  // RAMP (x=203..399)
  {
    char lbl[20]; snprintf(lbl, sizeof(lbl), "RAMP  %.1f%%", d.rampPct);
    canvas.setFont(&FreeMono9pt7b);
    canvas.setTextColor(1);
    canvas.setCursor(207, 229);
    canvas.print(lbl);

    // Bar x=207 y=234 w=186 h=14
    hbar(207, 234, 186, 14, d.rampPct, false);
  }

  canvas.drawFastHLine(0, 259, W, 1);

  // ══ ROW 2b: VEHICLE BATTERY  y=259..299 ══════════════════════════════════════
  ct(&FreeMono9pt7b, "VEHICLE BATTERY", 200, 266);

  {
    int bp = batPct(d.batV);
    char s[24]; snprintf(s, sizeof(s), "%.1fV  %d%%", d.batV, bp);
    battBar(5, 272, 389, 18, bp, s);
  }
}

// ─── Push canvas ──────────────────────────────────────────────────────────────
void pushCanvas() {
  uint8_t* buf = canvas.getBuffer();
  const int bpr = (W+7)/8;
  RlcdPort.RLCD_ColorClear(ColorWhite);
  for (int y = 0; y < H; y++) {
    uint8_t* row = buf + y*bpr;
    for (int b2 = 0; b2 < bpr; b2++) {
      uint8_t v = row[b2];
      for (int bit = 0; bit < 8; bit++) {
        int x = b2*8+bit; if (x >= W) break;
        if (v & (0x80 >> bit))
          RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, ColorBlack);
      }
    }
  }
  RlcdPort.RLCD_Display();
}

// ─── Demo ─────────────────────────────────────────────────────────────────────
DashData demoTick(unsigned long ms) {
  DashData d;
  float t = (ms % 10000) / 10000.f;
  if (t < 0.45f) {
    float p = t/0.45f;
    d.speedMph=p*26; d.rpm=350+p*1350; d.amps=8+p*62;
    d.setpointPct=80; d.livePct=p*80; d.rampPct=p*80;
    strncpy(d.state,"RAMPING",7);
  } else if (t < 0.65f) {
    d.speedMph=26; d.rpm=1700; d.amps=36;
    d.setpointPct=80; d.livePct=80; d.rampPct=80;
    strncpy(d.state,"HOLD",7);
  } else if (t < 0.72f) {
    float p=(t-0.65f)/0.07f;
    d.speedMph=26+p*2; d.rpm=1700+p*500; d.amps=60+p*35;
    d.setpointPct=100; d.livePct=80+p*20; d.rampPct=80+p*20;
    strncpy(d.state,"REENG",7);
  } else {
    float p=(t-0.72f)/0.28f;
    d.speedMph=28*(1-p); d.rpm=2200*(1-p)+200; d.amps=95*(1-p);
    d.setpointPct=100*(1-p); d.livePct=100*(1-p); d.rampPct=100*(1-p);
    strncpy(d.state,"IDLE",7);
  }
  d.batV = 25.4f - (ms/3600000.f)*2.f;
  if (d.batV < 20.f) d.batV = 25.4f;
  const char* modes[] = {"NORMAL","SPORT","ECO"};
  strncpy(d.mode, modes[(ms/12000)%3], 7);
  return d;
}

// ─── Setup / Loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  analogSetAttenuation(ADC_11db);
  if (CHRG_PIN >= 0) pinMode(CHRG_PIN, INPUT_PULLUP);
  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onReceive);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  }
  RlcdPort.RLCD_Init();
  Serial.println("Dashboard V8");
}

DashData      cur;
bool          live    = false;
unsigned long lastSig = 0, lastFrm = 0;

void loop() {
  unsigned long now = millis();
  if (now - lastFrm < 16) return;
  lastFrm = now;
  if (hasNew) {
    char snap[128];
    portENTER_CRITICAL(&mux); memcpy(snap,rxBuf,128); hasNew=false; portEXIT_CRITICAL(&mux);
    if (parsePacket(snap, cur)) { live=true; lastSig=now; }
  }
  bool demo = !live || (now-lastSig > 3000);
  if (demo) { cur=demoTick(now); if (live&&now-lastSig>3000) live=false; }
  drawDash(cur, demo);
  pushCanvas();
}
