// ════════════════════════════════════════════════════════════════════
//  RPM PIN TEST — standalone diagnostic, no ISR / no debounce / no RPM math
//
//  Purpose: isolate whether the ESP32 can see the RPM sensors toggle at
//  all, before trusting anything in the main greenpower_sender sketch.
//  Flash this BY ITSELF (separate sketch/upload from greenpower_sender).
//
//  How to use:
//   1. Flash this sketch, open Serial Monitor @ 115200.
//   2. With the sensor powered and nothing blocking the beam, note the
//      resting MOTOR/WHEEL values.
//   3. Wave something through each beam and watch for the value to flip.
//
//  If the value NEVER changes no matter what you do — even on this bare
//  sketch — the problem is upstream of any code: sensor power, ground
//  continuity, the physical wire to this pin, or (worth checking against
//  your board's schematic) a GPIO already used onboard for something else.
//
//  If it DOES flip here but not in the main sketch, the problem is in the
//  main sketch's interrupt/debounce logic, not the wiring — report back
//  the pin values you see and we'll fix that logic specifically.
//
//  Try both TEST_PIN_MODE values below (PULLUP and PULLDOWN) if you're not
//  sure which one matches your sensor's idle state.
// ════════════════════════════════════════════════════════════════════

#define MOTOR_RPM_PIN      4   // swap with WHEEL_RPM_PIN if this is backwards
#define WHEEL_RPM_PIN      3   // GPIO3 is a boot-strapping pin — fine as a normal
                                // input after boot, just don't be alarmed by a
                                // reading that looks odd for an instant at power-up
#define TEST_PIN_MODE     INPUT_PULLUP   // try INPUT_PULLDOWN if this looks wrong

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    pinMode(MOTOR_RPM_PIN, TEST_PIN_MODE);
    pinMode(WHEEL_RPM_PIN, TEST_PIN_MODE);

    Serial.println("\n[BOOT] RPM pin test");
    Serial.printf("  MOTOR = GPIO%d   WHEEL = GPIO%d   mode=%s\n",
                  MOTOR_RPM_PIN, WHEEL_RPM_PIN,
                  (TEST_PIN_MODE == INPUT_PULLUP) ? "INPUT_PULLUP" : "INPUT_PULLDOWN");
    Serial.printf("  Resting state — MOTOR=%d  WHEEL=%d\n",
                  digitalRead(MOTOR_RPM_PIN), digitalRead(WHEEL_RPM_PIN));
    Serial.println("  Now trigger each sensor by hand and watch for the values to flip...\n");
}

void loop() {
    static int lastMotor = -1;
    static int lastWheel = -1;

    int motor = digitalRead(MOTOR_RPM_PIN);
    int wheel = digitalRead(WHEEL_RPM_PIN);

    // Only print on change plus a heartbeat every ~1s, so a stuck pin is
    // obvious (repeating heartbeat, never a change line) instead of
    // scrolling identical lines forever.
    static uint32_t lastHeartbeat = 0;
    bool changed = (motor != lastMotor) || (wheel != lastWheel);
    bool heartbeat = millis() - lastHeartbeat >= 1000;

    if (changed) {
        Serial.printf("[%8lu ms] CHANGE   MOTOR=%d  WHEEL=%d\n", millis(), motor, wheel);
        lastMotor = motor;
        lastWheel = wheel;
    } else if (heartbeat) {
        Serial.printf("[%8lu ms] resting  MOTOR=%d  WHEEL=%d\n", millis(), motor, wheel);
        lastHeartbeat = millis();
    }

    delay(2);   // fast poll — this is a diagnostic, not the real RPM path
}
