#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── Display ─────────────────────────────────────────────────────────────────
// 128x64 SSD1306 — I2C remapped to TX/RX pins on Arduino Nano ESP32.
//   SDA → RX (D0, pin 0)
//   SCL → TX (D1, pin 1)
// Swap the two defines below if your wires are the other way around.
#define OLED_SDA 1   // TX pin
#define OLED_SCL 0   // RX pin
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ─── PWM (LEDC) config — ESP32, ~31kHz silent PWM ────────────────────────────
#define LEDC_CHANNEL  0
#define LEDC_FREQ_HZ  31372
#define LEDC_RES_BITS 8       // 8-bit resolution keeps output math (0-255) the same

// ─── Pins ────────────────────────────────────────────────────────────────────
const int PWM_PIN      = 9;
const int THROTTLE_PIN = A0;
const int ECO_PIN      = 4;
const int SPORT_PIN    = 5;
const int BUTTON_PIN   = 2;

// ─── Modes ───────────────────────────────────────────────────────────────────
enum Mode { ECO, NORMAL, SPORT };
Mode currentMode = NORMAL;

// ─── Tuning ──────────────────────────────────────────────────────────────────
// Climb rates: base %/ms before curve shaping.
//   ECO    — ~30s full ramp
//   NORMAL — ~15s full ramp
//   SPORT  — ~5s  full ramp
const float ECO_CLIMB_RATE    = 0.00333f;
const float NORMAL_CLIMB_RATE = 0.00667f;
const float SPORT_CLIMB_RATE  = 0.02000f;

// Re-engage climb rate: sport curve scaled to ~3s full journey.
const float REENGAGE_CLIMB_RATE = 0.03333f;

// Decay rates while trigger is released.
//   ECO    — 10s full decay
//   NORMAL — 7.5s full decay
//   SPORT  — 7.5s full decay
const float ECO_DECAY_RATE    = 0.01000f;
const float NORMAL_DECAY_RATE = 0.01333f;
const float SPORT_DECAY_RATE  = 0.01333f;

// ─── State machine ───────────────────────────────────────────────────────────
//   IDLE       — trigger not pressed, trackers decaying
//   REENGAGING — trigger re-pressed, climbing from 0 back to resumeTarget
//                using sport curve regardless of mode, then hands off to RAMPING
//   RAMPING    — climbing toward pot target using mode curve
//   HOLDING    — pot target reached, holding steady
enum State { IDLE, REENGAGING, RAMPING, HOLDING };
State currentState = IDLE;

// currentPwm   — live output value, 0..100%
// rampPwm      — ramp/re-engage position tracker
// rampStartPwm — rampPwm at the start of the current RAMPING segment;
//                progress is relative to this so the curve always starts slow
// resumeTarget — decayed speed at moment of re-press; sport re-engage ceiling
const float RESUME_THRESHOLD = 2.0f;
const float POT_ALPHA        = 0.2f;   // EMA smoothing — filters ±0.5% ADC jitter

float currentPwm   = 0.0f;
float rampPwm      = 0.0f;
float rampStartPwm = 0.0f;
float resumeTarget = 0.0f;
float smoothedPot  = 0.0f;

unsigned long lastTick    = 0;
bool          prevTrigger = false;

// ─── Display refresh ─────────────────────────────────────────────────────────
unsigned long lastDisplay = 0;

// ─── Curve multipliers ────────────────────────────────────────────────────────
// ECO main ramp: exponential exponent 1.8 — less bottom-heavy, quicker off line.
float ecoMultiplier(float progress) {
  return exp(1.8f * progress) / 2.047f;
}

// NORMAL main ramp: logistic S-curve centered at 0.15 — faster initial bite,
// eases off in the 70-100% range.
float normalMultiplier(float progress) {
  float sig = 1.0f / (1.0f + exp(-6.0f * (progress - 0.15f)));
  return sig * 1.5f + 0.15f;
}

// SPORT main ramp: sqrt — instant bite, flattens fast.
float sportMultiplier(float progress) {
  if (progress < 0.01f) return 5.0f;
  return min(0.5f / sqrt(progress), 5.0f);
}

// ─── Mode helpers ─────────────────────────────────────────────────────────────
float getClimbRate(Mode m) {
  switch (m) {
    case ECO:   return ECO_CLIMB_RATE;
    case SPORT: return SPORT_CLIMB_RATE;
    default:    return NORMAL_CLIMB_RATE;
  }
}

float getMultiplier(Mode m, float progress) {
  switch (m) {
    case ECO:   return ecoMultiplier(progress);
    case SPORT: return sportMultiplier(progress);
    default:    return normalMultiplier(progress);
  }
}

float getDecayRate(Mode m) {
  switch (m) {
    case SPORT: return SPORT_DECAY_RATE;
    case ECO:   return ECO_DECAY_RATE;
    default:    return NORMAL_DECAY_RATE;
  }
}

// ─── Utility ─────────────────────────────────────────────────────────────────
float clamp(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

const char* modeName(Mode m) {
  switch (m) { case ECO: return "ECO"; case SPORT: return "SPORT"; default: return "NORMAL"; }
}

const char* stateName(State s) {
  switch (s) {
    case IDLE:       return "IDLE";
    case REENGAGING: return "REENG";
    case RAMPING:    return "RAMP";
    default:         return "HOLD";
  }
}

// ─── Display update ───────────────────────────────────────────────────────────
void updateDisplay(float potPct, float outputPct) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Row 1: mode (left) | state (right) ───────────────────────────────────
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(modeName(currentMode));
  const char* sn = stateName(currentState);
  display.setCursor(128 - (int)strlen(sn) * 6, 0);
  display.print(sn);

  // ── Separator ─────────────────────────────────────────────────────────────
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  // ── Output value — size 3, centred ───────────────────────────────────────
  display.setTextSize(3);
  // Each char is 18px wide at size 3; pick column count to centre
  int chars = (outputPct >= 100.0f) ? 6 : (outputPct >= 10.0f) ? 5 : 4;
  display.setCursor(max(0, (128 - chars * 18) / 2), 13);
  display.print(outputPct, 1);
  display.print("%");

  // ── Separator ─────────────────────────────────────────────────────────────
  display.drawFastHLine(0, 38, 128, SSD1306_WHITE);

  // ── Bottom rows: pot / ramp ───────────────────────────────────────────────
  display.setTextSize(1);

  display.setCursor(0, 42);
  display.print("Pot:  ");
  display.print(potPct, 1);
  display.print("%");

  display.setCursor(0, 54);
  display.print("Ramp: ");
  display.print(rampPwm, 1);
  display.print("%");

  display.display();
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);  // ~31kHz silent PWM
  ledcAttachPin(PWM_PIN, LEDC_CHANNEL);
  pinMode(ECO_PIN,    INPUT_PULLUP);
  pinMode(SPORT_PIN,  INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  Serial.println("Throttle controller ready. V17");

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found — check wiring.");
    while (true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Throttle Ctrl V17");
  display.display();

  lastTick    = millis();
  lastDisplay = millis();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  float dt = (float)(now - lastTick);
  lastTick = now;

  // ── Mode selection ──────────────────────────────────────────────────────────
  // Active-low switches. ECO wins if both pulled low.
  // Mode switch leaves currentPwm and rampPwm untouched — no output jump.
  Mode newMode = (digitalRead(ECO_PIN)   == LOW) ? ECO   :
                 (digitalRead(SPORT_PIN) == LOW) ? SPORT : NORMAL;
  if (newMode != currentMode) {
    currentMode = newMode;
    Serial.print("Mode -> "); Serial.println(modeName(currentMode));
  }

  // ── Pot — target speed ──────────────────────────────────────────────────────
  // Pot sets the ceiling the ramp climbs toward.
  // Pot down → output drops immediately. Pot up → ramp resumes.
  float rawPot  = (analogRead(THROTTLE_PIN) / 4095.0f) * 100.0f;  // 12-bit ADC on ESP32
  if (rawPot > 99.0f) rawPot = 100.0f;  // snap to 100% — pots rarely reach true ADC max
  if (rawPot <  1.0f) rawPot =   0.0f;  // snap to 0% at the low end too
  smoothedPot   = POT_ALPHA * rawPot + (1.0f - POT_ALPHA) * smoothedPot;
  float potPct  = smoothedPot;

  // ── Trigger edge detection ──────────────────────────────────────────────────
  bool triggerHeld  = (digitalRead(BUTTON_PIN) == LOW);
  bool justPressed  = triggerHeld  && !prevTrigger;
  bool justReleased = !triggerHeld && prevTrigger;
  prevTrigger = triggerHeld;

  // ── State transitions ───────────────────────────────────────────────────────
  if (justReleased) {
    currentState = IDLE;
  }

  if (justPressed) {
    if (currentPwm > RESUME_THRESHOLD) {
      // Sport re-engage: climb from 0 back to wherever currentPwm has decayed
      // to, using sport curve regardless of mode. Hands off to mode curve after.
      resumeTarget = currentPwm;
      rampPwm      = 0.0f;
      currentPwm   = 0.0f;
      currentState = REENGAGING;
    } else {
      // Fully decayed — fresh ramp from zero using mode curve.
      rampStartPwm = 0.0f;
      rampPwm      = 0.0f;
      currentPwm   = 0.0f;
      currentState = RAMPING;
    }
  }

  // ── State machine ───────────────────────────────────────────────────────────
  switch (currentState) {

    case IDLE: {
      // Decay both trackers so currentPwm is ready as resumeTarget on re-press.
      // Output is forced to 0 by trigger safety below.
      float drop = getDecayRate(currentMode) * dt;
      rampPwm    = clamp(rampPwm    - drop, 0.0f, 100.0f);
      currentPwm = clamp(currentPwm - drop, 0.0f, 100.0f);
      break;
    }

    case REENGAGING: {
      // Sport curve climbs rampPwm from 0 up to resumeTarget.
      // Progress is position-based so the curve shape is identical to a sport
      // ramp — fast bite that flattens as it approaches the target.
      float progress = clamp(rampPwm / 100.0f, 0.0f, 1.0f);
      float step     = REENGAGE_CLIMB_RATE * sportMultiplier(progress) * dt;
      rampPwm        = clamp(rampPwm + step, 0.0f, resumeTarget);
      currentPwm     = rampPwm;

      if (rampPwm >= resumeTarget - 0.1f) {
        rampPwm      = resumeTarget;
        currentPwm   = resumeTarget;
        rampStartPwm = resumeTarget;
        currentState = RAMPING;
        Serial.println("[REENG -> RAMP]");
      }
      break;
    }

    case RAMPING: {
      // Climb toward pot target using mode curve.
      // Progress relative to rampStartPwm so every segment starts from the
      // slow end of the curve, whether fresh or handed off from REENGAGING.
      if (rampPwm >= potPct) {
        currentPwm   = potPct;
        rampPwm      = potPct;
        currentState = HOLDING;
        break;
      }
      float progress = clamp((rampPwm - rampStartPwm) / 100.0f, 0.0f, 1.0f);
      float step     = getClimbRate(currentMode) * getMultiplier(currentMode, progress) * dt;
      rampPwm    = clamp(rampPwm + step, 0.0f, potPct);
      currentPwm = rampPwm;
      if (rampPwm >= potPct) currentState = HOLDING;
      break;
    }

    case HOLDING: {
      // Track pot movement. Drop follows immediately, rise resumes ramp.
      if (potPct < rampPwm - 0.5f) {
        currentPwm = potPct;
        rampPwm    = potPct;
      } else if (potPct > rampPwm + 0.5f) {
        rampStartPwm = rampPwm;
        currentState = RAMPING;
      }
      break;
    }
  }

  // ── Output ──────────────────────────────────────────────────────────────────
  // Hard zero if trigger not held — always safe regardless of state.
  float outputPct = triggerHeld ? clamp(currentPwm, 0.0f, 100.0f) : 0.0f;
  ledcWrite(LEDC_CHANNEL, (int)(outputPct / 100.0f * 255.0f));

  // ── Serial telemetry ─────────────────────────────────────────────────────────
  Serial.print(modeName(currentMode));   Serial.print(" | ");
  Serial.print(stateName(currentState)); Serial.print(" | ");
  Serial.print("pot:");    Serial.print(potPct,       1); Serial.print("% ");
  Serial.print("ramp:");   Serial.print(rampPwm,      1); Serial.print("% ");
  Serial.print("resume:"); Serial.print(resumeTarget, 1); Serial.print("% ");
  Serial.print("out:");    Serial.print(outputPct,    1); Serial.println("%");

  // ── Display update — every 100ms ─────────────────────────────────────────────
  if (now - lastDisplay >= 100) {
    lastDisplay = now;
    updateDisplay(potPct, outputPct);
  }

  delay(20);
}
