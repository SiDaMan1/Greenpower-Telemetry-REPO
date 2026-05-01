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

// ── Screen battery (18650) ───────────────────────────────────────────────────
// Verify SCR_BATT_PIN from the Waveshare RLCD 4.2" dev board schematic/silkscreen.
// GPIO1 is the most common sense pin on this module (1:2 voltage divider, Vadc = Vbat/2).
#define SCR_BATT_PIN 1
#define SCR_BATT_DIV 2.0f   // Vbat = Vadc × this ratio

DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1  canvas(W, H);

// ═══════════════════════════════════════════════════════════════
//  LAYOUT
//
//  ROW 0  y =  0 ..  29   Header
//    Mode badge    x=3,   w=90,  h=24
//    State badge   x=97,  w=108, h=24
//    Vehicle bat   x=209, w=186, h=12, y=2  (top bar)
//    Screen bat    x=209, w=186, h=12, y=16 (bottom bar)
//
//  ROW 1  y = 30 .. 209   Gauges
//    Speed  CX=97  CY=120 R=72   | x=0..199
//    RPM    CX=303 CY=120 R=72   | x=200..399
//
//  ROW 2  y = 210 .. 299  Info
//    Amps   x=0..109
//    Values x=110..399  (SET/LIVE/RAMP)
// ═══════════════════════════════════════════════════════════════

// ─── Data ────────────────────────────────────────────────────────────────────
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

// ─── ESP-NOW ─────────────────────────────────────────────────────────────────
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

// ─── Helpers ─────────────────────────────────────────────────────────────────
int batPct(float v) {
  int p = (int)((v - 20.0f) / 5.6f * 100.0f);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

int scrBatPct() {
  float vadc = analogRead(SCR_BATT_PIN) / 4095.0f * 3.3f;
  float vbat = vadc * SCR_BATT_DIV;
  int p = (int)((vbat - 3.0f) / 1.2f * 100.0f);  // 3.0V=0%, 4.2V=100%
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

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

// Centre text horizontally and vertically on (px, py)
void ct(const GFXfont* f, const char* s, int px, int py) {
  canvas.setFont(f);
  canvas.setTextColor(1);
  int16_t x1, y1; uint16_t w, h;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor(px - (int)w/2, py + (int)h/2);
  canvas.print(s);
}

// Filled inverted badge
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

// Thin horizontal battery bar (h=12) with label text centred inside
void battBar(int x, int y, int w, int h, int pct, const char* label) {
  canvas.drawRect(x, y, w, h, 1);
  canvas.fillRect(x+w, y+h/2-2, 3, 4, 1);          // nub
  int f = (int)((w-2) * pct / 100);
  if (f > 0) canvas.fillRect(x+1, y+1, f, h-2, 1); // fill
  // label — baseline sits 3px above bar bottom so ascent lands inside
  canvas.setFont(&FreeMono9pt7b);
  canvas.setTextColor(pct > 50 ? 0 : 1);
  int16_t x1_, y1_; uint16_t tw, th;
  canvas.getTextBounds(label, 0, 0, &x1_, &y1_, &tw, &th);
  canvas.setCursor(x + w/2 - (int)tw/2, y + h - 3);
  canvas.print(label);
  canvas.setTextColor(1);
}

void minibar(int x, int y, int w, int h, float pct) {
  canvas.drawRect(x, y, w, h, 1);
  int f = (int)((w-2) * (pct > 100.f ? 1.f : pct / 100.f));
  if (f > 0) canvas.fillRect(x+1, y+1, f, h-2, 1);
}

// ─── Gauge ───────────────────────────────────────────────────────────────────
void drawGauge(int cx, int cy, int r,
               float val, float lo, float hi,
               const char* bigNum, const char* unit,
               int dangerLo, int dangerHi,
               int effAt,
               int tickStep, int labelStep) {

  // Outer arc
  arc(cx, cy, r,   -45.f, 225.f, 2);
  arc(cx, cy, r-4, -45.f, 225.f, 1);

  // Danger zone hatch
  if (dangerLo > 0) {
    float dLo_d  = 225.f - ((float)dangerLo / hi) * 270.f;
    float dHi_d  = 225.f - ((float)dangerHi / hi) * 270.f;
    float dStart = dHi_d < dLo_d ? dHi_d : dLo_d;
    float dEnd   = dHi_d < dLo_d ? dLo_d : dHi_d;
    for (float dg = dStart; dg <= dEnd; dg += 0.9f) {
      float a = dg * (float)M_PI / 180.f;
      for (int ri = r-9; ri <= r+2; ri += 2)
        canvas.drawPixel(cx+(int)(ri*cosf(a)), cy-(int)(ri*sinf(a)), 1);
    }
  }

  // Progress fill arc
  float fillEnd = 225.f - (val / hi) * 270.f;
  if (val > lo + 0.5f) arc(cx, cy, r-9, fillEnd, 225.f, 3);

  // Efficiency marker
  if (effAt > 0) {
    float ed = 225.f - ((float)effAt / hi) * 270.f;
    tick(cx, cy, r+4, r-14, ed);
    tick(cx, cy, r+4, r-14, ed+2.f);
  }

  // Ticks + labels — skip label at right end-stop (-45°) so it can't clash with the centre number
  for (float v = lo; v <= hi + 0.1f; v += tickStep) {
    float deg = 225.f - ((v - lo) / (hi - lo)) * 270.f;
    bool  isMajor = (((int)v % labelStep) == 0);
    tick(cx, cy, r, isMajor ? r-15 : r-7, deg);
    if (isMajor && deg > -40.f) {
      char buf[8];
      if (hi >= 1000) {
        if ((int)v == 0) snprintf(buf, sizeof(buf), "0");
        else             snprintf(buf, sizeof(buf), "%dk", (int)v/1000);
      } else {
        snprintf(buf, sizeof(buf), "%d", (int)v);
      }
      float a  = deg * (float)M_PI / 180.f;
      int   lx = cx + (int)((r-26)*cosf(a));
      int   ly = cy - (int)((r-26)*sinf(a));
      ct(&FreeMono9pt7b, buf, lx, ly);
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

  // Centre number (18pt — fits inside arc without clipping)
  ct(&FreeMonoBold18pt7b, bigNum, cx, cy+20);
  ct(&FreeMono9pt7b,      unit,   cx, cy+46);
}

// ─── Full dashboard ───────────────────────────────────────────────────────────
void drawDash(const DashData& d, bool demo) {
  canvas.fillScreen(0);

  // ══ ROW 0: HEADER  y=0..29 ══════════════════════════════════════════════════
  badge(3,  3,  90, 24, d.mode);
  badge(97, 3, 108, 24, d.state);

  // Vehicle battery — top thin bar
  {
    int bp = batPct(d.batV);
    char s[20]; snprintf(s, sizeof(s), "%.1fV %d%%", d.batV, bp);
    battBar(209, 2, 186, 12, bp, s);
  }

  // Screen (18650) battery — bottom thin bar
  {
    int sp = scrBatPct();
    char s[20];
    if (demo) snprintf(s, sizeof(s), "SCR %d%% DM", sp);
    else      snprintf(s, sizeof(s), "SCR %d%%",    sp);
    battBar(209, 16, 186, 12, sp, s);
  }

  canvas.drawFastHLine(0, 30, W, 1);

  // ══ ROW 1: GAUGES  y=30..209 ════════════════════════════════════════════════
  canvas.drawFastVLine(200, 30, 180, 1);
  ct(&FreeMono9pt7b, "SPEED",     97,  41);
  ct(&FreeMono9pt7b, "MOTOR RPM", 303, 41);

  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.1f", d.speedMph);
    drawGauge(97, 120, 72, d.speedMph, 0, 30, buf, "MPH", 0, 0, 0, 5, 10);
  }
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.0f", d.rpm);
    drawGauge(303, 120, 72, d.rpm, 0, 2500, buf, "RPM", 2200, 2500, 1700, 250, 1000);
  }

  canvas.drawFastHLine(0, 210, W, 1);

  // ══ ROW 2: INFO  y=210..299 ═════════════════════════════════════════════════
  canvas.drawFastVLine(110, 210, 90, 1);

  // ── Amps (x=0..109) ─────────────────────────────────────────────────────────
  ct(&FreeMono9pt7b, "AMPS", 54, 219);

  // Large amp value
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%.0fA", d.amps);
    ct(&FreeMonoBold18pt7b, buf, 54, 238);
  }

  // Horizontal level bar  x=6 y=253 w=98 h=16
  {
    const int BX=6, BY=253, BW=98, BH=16;
    canvas.drawRect(BX, BY, BW, BH, 1);

    // 80A warning line
    int warnX = BX + 1 + (int)((BW-2) * 0.8f);
    canvas.drawFastVLine(warnX, BY, BH, 1);

    // Fill
    float ap   = d.amps > 100.f ? 1.f : d.amps / 100.f;
    int   fill = (int)((BW-2) * ap);
    if (fill > 0) {
      if (d.amps > 80.f) {
        // Hatch above warning threshold
        for (int fy = BY+1; fy < BY+BH-1; fy++)
          for (int fx = BX+1; fx < BX+1+fill; fx++)
            if ((fx+fy) % 2 == 0) canvas.drawPixel(fx, fy, 1);
      } else {
        canvas.fillRect(BX+1, BY+1, fill, BH-2, 1);
      }
    }

    // Scale labels below bar
    canvas.setFont(&FreeMono9pt7b);
    canvas.setTextColor(1);
    canvas.setCursor(BX,      BY+BH+9); canvas.print("0");
    canvas.setCursor(BX+BW-6, BY+BH+9); canvas.print("^");  // marks 80A warn line
    canvas.setCursor(warnX-3, BY+BH+9); canvas.print("80");
  }

  // ── SET / LIVE / RAMP rows (x=110..399) ─────────────────────────────────────
  const int RX = 114, RW = 282;
  struct { const char* lbl; float val; int ly; int by; } rows[3] = {
    { "SET",  d.setpointPct, 224, 229 },
    { "LIVE", d.livePct,     252, 257 },
    { "RAMP", d.rampPct,     280, 285 },
  };
  for (int i = 0; i < 3; i++) {
    char txt[20];
    snprintf(txt, sizeof(txt), "%s  %.1f%%", rows[i].lbl, rows[i].val);
    canvas.setFont(&FreeMono9pt7b);
    canvas.setTextColor(1);
    canvas.setCursor(RX, rows[i].ly);
    canvas.print(txt);
    minibar(RX, rows[i].by, RW, 10, rows[i].val);
  }
}

// ─── Push canvas ─────────────────────────────────────────────────────────────
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

// ─── Demo ────────────────────────────────────────────────────────────────────
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
  analogSetAttenuation(ADC_11db);  // full 0-3.3V range for battery ADC
  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onReceive);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  }
  RlcdPort.RLCD_Init();
  Serial.println("Dashboard V7");
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
