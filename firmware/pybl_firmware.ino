/**
 * Pybl Meditation Pebble — Main Firmware
 * =======================================
 * Seeed XIAO nRF52840 + DRV2605L haptic driver + LRA motor
 *
 * Behaviour:
 *   - Device boots into deep sleep (~5µA draw)
 *   - Touch on TOUCH_PIN wakes the device and starts a breathing session
 *   - Another touch (or SESSION_TIMEOUT_MS elapsing) ends the session
 *   - Device plays a haptic confirmation and returns to deep sleep
 *   - Low battery triggers a warning stutter then a clean shutdown
 *
 * Wiring:
 *   XIAO D4  → DRV2605L SDA
 *   XIAO D5  → DRV2605L SCL
 *   XIAO D6  → DRV2605L EN  (enables/disables the driver to save power)
 *   XIAO D7  → TTP223 signal (touch input, pulled LOW at rest, HIGH on touch)
 *   XIAO 3V3 → DRV2605L VIN, TTP223 VCC
 *   XIAO GND → DRV2605L GND, TTP223 GND
 */

#include <Wire.h>
#include <math.h>
#include "Adafruit_DRV2605.h"
#include <nrfx_power.h>   // needed for sd_power_system_off() deep sleep

// ─── Pin definitions ──────────────────────────────────────────────────────────

#define SDA_PIN        4   // XIAO D4
#define SCL_PIN        5   // XIAO D5
#define DRV_ENABLE_PIN 6   // DRV2605L EN — HIGH enables the chip, LOW = standby
#define TOUCH_PIN      7   // TTP223 output — HIGH when finger detected

// Battery ADC pins (XIAO nRF52840 specific)
#define BATT_ENABLE_PIN  P0_14   // Pull HIGH to connect the voltage divider
#define BATT_ADC_PIN     P0_31   // Read divided battery voltage here

// ─── Breathing timing (milliseconds) ─────────────────────────────────────────

const uint32_t INHALE_MS        = 6000;
const uint32_t TOP_HOLD_MS      = 1000;
const uint32_t EXHALE_MS        = 6000;
const uint32_t BOTTOM_HOLD_MS   = 1000;
const uint32_t TOTAL_PERIOD_MS  = INHALE_MS + TOP_HOLD_MS + EXHALE_MS + BOTTOM_HOLD_MS;

// ─── Haptic amplitude ────────────────────────────────────────────────────────

const uint8_t  MAX_AMPLITUDE    = 75;
const uint8_t  MIN_AMPLITUDE    = 10;
const uint8_t  AMPLITUDE_RANGE  = MAX_AMPLITUDE - MIN_AMPLITUDE;
const uint32_t UPDATE_INTERVAL_MS = 20;   // how often we push a new value to DRV2605L

// ─── Session settings ────────────────────────────────────────────────────────

// Auto-end a session after this long even if the user doesn't touch again.
// 20 minutes is a reasonable default for a meditation session.
const uint32_t SESSION_TIMEOUT_MS = 20UL * 60UL * 1000UL;

// ─── Battery thresholds (millivolts) ─────────────────────────────────────────

// LiPo safe operating range is roughly 3500–4200mV.
// We warn at 3600mV and shut down at 3400mV to protect the cell.
const uint16_t BATT_WARN_MV     = 3600;
const uint16_t BATT_CUTOFF_MV   = 3400;

// ─── Global state ────────────────────────────────────────────────────────────

Adafruit_DRV2605 drv;

// Tracks whether we're currently in an active breathing session
volatile bool sessionActive = false;

// Tracks the last time we updated the haptic output
uint32_t lastHapticUpdate = 0;

// Tracks when the current session started (for auto-timeout)
uint32_t sessionStartMs = 0;


// ─── Interrupt service routine ───────────────────────────────────────────────

/**
 * Called by hardware when the TTP223 touch pin goes HIGH.
 * We keep this as short as possible — just set a flag.
 * The main loop reads the flag and acts on it.
 *
 * volatile ensures the compiler doesn't optimise away reads of sessionActive
 * in the main loop, since it can change outside normal program flow.
 */
void wakeISR() {
    sessionActive = !sessionActive;   // toggle: touch starts or stops a session
}


// ─── Battery monitoring ───────────────────────────────────────────────────────

/**
 * Reads the LiPo cell voltage in millivolts.
 *
 * The XIAO routes battery voltage through a 1:2 voltage divider gated by
 * BATT_ENABLE_PIN. We must enable the divider, wait for it to settle,
 * read the ADC, then disable it again — leaving it enabled would draw
 * continuous current and undermine the deep sleep power budget.
 *
 * The ADC is 12-bit (0–4095) referenced to 3.3V.
 * Divider halves the voltage, so: actual_mv = (raw / 4095) * 3300 * 2
 */
uint16_t readBatteryMillivolts() {
    pinMode(BATT_ENABLE_PIN, OUTPUT);
    digitalWrite(BATT_ENABLE_PIN, HIGH);
    delay(2);   // allow voltage divider to settle before sampling

    analogReadResolution(12);   // ensure we're using full 12-bit resolution
    uint16_t raw = analogRead(BATT_ADC_PIN);

    digitalWrite(BATT_ENABLE_PIN, LOW);   // disable divider immediately after read

    return (uint16_t)((raw / 4095.0f) * 3300.0f * 2.0f);
}

/**
 * Checks battery voltage and takes action if necessary.
 * Call this at the start of each session, not continuously —
 * the ADC enable/disable cycle draws current and shouldn't run in a tight loop.
 *
 * Returns false if battery is critically low and the device should shut down.
 */
bool checkBattery() {
    uint16_t battMv = readBatteryMillivolts();

    if (battMv <= BATT_CUTOFF_MV) {
        playLowBatteryWarning();
        return false;   // signal to caller: shut down now
    }

    if (battMv <= BATT_WARN_MV) {
        // Warn the user but allow the session to continue
        playLowBatteryWarning();
    }

    return true;
}


// ─── Haptic feedback patterns ─────────────────────────────────────────────────

/**
 * Enables the DRV2605L by pulling its EN pin HIGH, then waits briefly
 * for the chip to power up before we try to communicate over I2C.
 */
void enableDriver() {
    digitalWrite(DRV_ENABLE_PIN, HIGH);
    delay(10);   // DRV2605L needs ~1ms to wake; 10ms gives comfortable margin
}

/**
 * Disables the DRV2605L by pulling EN LOW, putting it into standby.
 * In standby the chip draws ~10µA instead of its active ~7mA.
 * Always call this before entering deep sleep.
 */
void disableDriver() {
    drv.setRealtimeValue(0);   // ensure motor is stopped before cutting power
    digitalWrite(DRV_ENABLE_PIN, LOW);
}

/**
 * Plays two short soft pulses to confirm the device has woken successfully.
 * Gives the user tactile feedback that their touch was registered.
 */
void playWakeConfirmation() {
    for (int pulse = 0; pulse < 2; pulse++) {
        drv.setRealtimeValue(60);
        delay(80);
        drv.setRealtimeValue(0);
        delay(120);
    }
}

/**
 * Plays a slow fade-out to signal the session is ending intentionally.
 * Feels deliberate rather than abrupt, which matters for a meditation device.
 */
void playSleepConfirmation() {
    for (uint8_t amplitude = 60; amplitude > 0; amplitude -= 3) {
        drv.setRealtimeValue(amplitude);
        delay(30);
    }
    drv.setRealtimeValue(0);
}

/**
 * Plays three sharp short buzzes to indicate low battery.
 * A deliberately distinct pattern from wake/sleep so the user can
 * tell something different is happening.
 */
void playLowBatteryWarning() {
    for (int buzz = 0; buzz < 3; buzz++) {
        drv.setRealtimeValue(80);
        delay(60);
        drv.setRealtimeValue(0);
        delay(100);
    }
}


// ─── Breathing envelope ───────────────────────────────────────────────────────

/**
 * Returns a smoothed intensity value (0.0 to 1.0) for a given linear
 * progress value t (also 0.0 to 1.0).
 *
 * This is smoothstep (cubic Hermite) fed into a power curve.
 * The result is a shape that eases in slowly, accelerates through the
 * middle, and eases out — much closer to how a natural breath feels
 * than a straight linear ramp, which feels robotic.
 *
 * Example: at t=0.0 → 0.0, t=0.5 → 0.5, t=1.0 → 1.0,
 * but the curve through those points is S-shaped rather than straight.
 */
float getOrganicIntensity(float t) {
    float smoothT = t * t * (3.0f - 2.0f * t);   // smoothstep
    return pow(smoothT, 2.0f);                     // gentle power curve on top
}

/**
 * Computes the correct haptic amplitude for the current moment in the
 * breathing cycle and sends it to the DRV2605L.
 *
 * Should be called every UPDATE_INTERVAL_MS milliseconds during a session.
 * Uses absolute time (millis()) relative to the session start so the
 * cycle position stays accurate even if there's any processing delay.
 */
void updateBreathingEnvelope() {
    uint32_t currentMs  = millis();
    uint32_t sessionMs  = currentMs - sessionStartMs;
    uint32_t cycleTime  = sessionMs % TOTAL_PERIOD_MS;
    float    intensity  = 0.0f;

    if (cycleTime < INHALE_MS) {
        // Inhale phase: amplitude rises 0 → 1
        float t = (float)cycleTime / (float)INHALE_MS;
        intensity = getOrganicIntensity(t);

    } else if (cycleTime < INHALE_MS + TOP_HOLD_MS) {
        // Top hold: amplitude stays at peak
        intensity = 1.0f;

    } else if (cycleTime < INHALE_MS + TOP_HOLD_MS + EXHALE_MS) {
        // Exhale phase: amplitude falls 1 → 0
        uint32_t exhaleTime = cycleTime - (INHALE_MS + TOP_HOLD_MS);
        float t = 1.0f - ((float)exhaleTime / (float)EXHALE_MS);
        intensity = getOrganicIntensity(t);

    } else {
        // Bottom hold: silence
        intensity = 0.0f;
    }

    uint8_t amplitude = MIN_AMPLITUDE + (uint8_t)(intensity * AMPLITUDE_RANGE);
    drv.setRealtimeValue(amplitude);
}


// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    // Set charge current to 50mA — gentle for a small cell in a sealed enclosure.
    // HICHG HIGH = 100mA, HICHG LOW = 50mA.
    pinMode(P0_13, OUTPUT);
    digitalWrite(P0_13, LOW);

    // DRV2605L enable pin — start LOW (driver off) until a session begins
    pinMode(DRV_ENABLE_PIN, OUTPUT);
    digitalWrite(DRV_ENABLE_PIN, LOW);

    // Touch pin — input with internal pull-down so it reads LOW at rest
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);

    // Attach interrupt so a touch wakes the device from deep sleep.
    // RISING = fire when the pin goes from LOW to HIGH (finger lands on sensor).
    attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), wakeISR, RISING);

    // Start I2C on the correct XIAO pins before talking to the DRV2605L
    Wire.begin(SDA_PIN, SCL_PIN);

    // Initial deep sleep — device sits here drawing ~5µA until first touch
    enterDeepSleep();
}


// ─── Deep sleep ───────────────────────────────────────────────────────────────

/**
 * Puts the nRF52840 into System Off mode — the deepest available sleep.
 * Current draw drops to ~5µA (mostly the always-on voltage reference).
 *
 * The device wakes ONLY via a GPIO interrupt (our touch pin).
 * On wake, execution restarts from the top of setup(), not from here —
 * System Off is a full reset, not a resume. The sessionActive flag set
 * by wakeISR() survives in retained RAM across this reset.
 *
 * Always disable the DRV2605L before calling this.
 */
void enterDeepSleep() {
    disableDriver();
    // sd_power_system_off() is the nRF52840's System Off call.
    // It does not return — the next line the CPU executes is setup()
    // after the interrupt fires.
    sd_power_system_off();
}


// ─── Main loop ────────────────────────────────────────────────────────────────

void loop() {
    if (sessionActive) {
        // ── Session start ──────────────────────────────────────────────────

        // Check battery before committing to a session.
        // If critically low, checkBattery() plays the warning and returns false.
        if (!checkBattery()) {
            enterDeepSleep();
            return;
        }

        // Power up the DRV2605L and run auto-calibration for this LRA.
        // Calibration takes ~1 second and tunes the resonant frequency
        // tracking to your specific motor — makes a real difference to feel.
        enableDriver();
        drv.begin();
        drv.useLRA();
        drv.autoCalibrate();          // tunes to your specific LRA
        drv.setMode(DRV2605_MODE_REALTIME);

        // Confirm wake to the user with two short pulses
        playWakeConfirmation();

        // Record when this session started for timeout and cycle calculations
        sessionStartMs = millis();
        lastHapticUpdate = millis();

        // ── Session loop ───────────────────────────────────────────────────

        while (sessionActive) {
            uint32_t now = millis();

            // Auto-timeout: end session if it's been running too long
            if (now - sessionStartMs >= SESSION_TIMEOUT_MS) {
                sessionActive = false;
                break;
            }

            // Push a new amplitude to the DRV2605L every UPDATE_INTERVAL_MS
            if (now - lastHapticUpdate >= UPDATE_INTERVAL_MS) {
                lastHapticUpdate = now;
                updateBreathingEnvelope();
            }

            // Check for a second touch (to end the session early).
            // The ISR already toggled sessionActive — we just need to
            // let the while() condition above catch it naturally.
            // A small yield here prevents hammering the CPU in a tight loop.
            delay(1);
        }

        // ── Session end ────────────────────────────────────────────────────

        playSleepConfirmation();
        enterDeepSleep();   // back to ~5µA until next touch

    } else {
        // Should rarely reach here since setup() ends in enterDeepSleep(),
        // but if it does, go back to sleep immediately.
        enterDeepSleep();
    }
}