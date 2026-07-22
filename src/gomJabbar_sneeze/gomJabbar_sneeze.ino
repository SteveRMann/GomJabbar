// ============================================================
// Alien Spitter Prop - Wemos D1 Mini (ESP8266)
// ============================================================
// TOF proximity trigger sequence:
//   1. Eyes ramp blue-green(idle) -> red over 1s, with an
//      accelerating warning trill building tension
//   2. 3x { sneeze tone burst + pump pulse, simultaneously }
//   3. Eyes fade red -> black over 1s, then fade black -> idle breathing over 1s
//   4. Trigger cannot fire again until QUIET_MS has passed
//
// Button on D3: NOT a sneeze trigger. Holding it down runs the
// pump for PRIME_PUMP_MS (5s) to prime the ~15ft of tubing
// between the pump and the prop before use.
//
// D1/D2 (GPIO5/GPIO4) are reserved for a future I2C gesture
// sensor and are NOT used in this sketch.
// ============================================================

#include <Arduino.h>
#include "Adafruit_VL53L0X.h"

Adafruit_VL53L0X lox = Adafruit_VL53L0X();


// ---------------- Pin assignments ----------------
const int sclPin = D1;
const int sdaPin = D2;
const int BUTTON_PIN = D3;  // button to GND, INPUT_PULLUP
const int PUMP_PIN = D4;    // MOSFET gate, active HIGH
const int EYE_B_PIN = D5;
const int EYE_G_PIN = D6;
const int EYE_R_PIN = D7;
const int SPEAKER_PIN = D8;  // tone() output


// --------------------------------------------------------------------------------
// ------------------------------------ Tunable -----------------------------------
// --------------------------------------------------------------------------------
const unsigned long BREATH_PERIOD_MS = 7000;  // one idle brightness breathe cycle
const unsigned long COLOR_PERIOD_MS = 16000;  // one blue<->green color drift cycle (deliberately not a clean multiple of BREATH_PERIOD_MS so the two never lock into a repeating pattern)
const int IDLE_MAX_BRIGHTNESS = 700;          // of 1023, idle eye ceiling
const int IDLE_MIN_BRIGHTNESS = 180;          // of 1023, idle eye floor -- never fully off

const unsigned long RAMP_MS = 3000;           // idle-color -> red ramp

// ---------------- Warning trill (ramp charge-up sound) ----------------
const unsigned long CHIRP_INTERVAL_START = 450;  // ms between trills at ramp start (slow)
const unsigned long CHIRP_INTERVAL_END = 60;     // ms between trills at ramp end (fast)
const float CHIRP_EASE_POWER = 2.2;              // >1 = stays slow longer, then rushes near the end

const int SNEEZE_REPS = 3;
const unsigned long SNEEZE_GAP_MS = 400;  // pause between reps
const unsigned long PUMP_PULSE_MS = 50;   // pump ON time per sneeze pulse
const unsigned long PRIME_PUMP_MS = 5000;  // pump ON time for button-triggered tube priming
const unsigned long FADE_OUT_MS = 1000;   // red -> black
const unsigned long FADE_IN_MS = 1000;    // black -> idle breathing

const int sneezeZoneMin = 200;           // Minimum distance (mm) to TOF sensor. Anything greater will trigger a sneeze
const int sneezeZoneMax = 8000;          // Maximum distance to trigger a sneeze. Anything greater is ignored.
const unsigned long tofDebounce = 500;   // ms the target must be continuously in-zone before a TOF trigger fires
const unsigned long QUIET_MS = 2000;  // cooldown after a sequence ends before button OR TOF can trigger the next one

const unsigned long DEBOUNCE_MS = 30;

// ---------------- Spit sound (chaotic, regenerated per rep) ----------------
// Ported from the Water Watcher test sketch's waterWatcherAlienSpit().
// Segment A emits a short 8ms tone "pluck" followed by a variable silence
// gap (matching the original's tone(freq, 8) + delay(random(8,14)) pairing
// exactly). Segments B and C had equal tone-duration/delay in the original,
// so they play back-to-back with no gap. generateSpitSequence() runs fresh
// at the start of every rep.
const int SPIT_SEG_A_STEPS = 10;  // irregular high-frequency charge
const int SPIT_SEG_B_STEPS = 18;  // chaotic dual-tone beating
const int SPIT_SEG_C_STEPS = 12;  // low-frequency expulsion
const int MAX_SPIT_STEPS = (SPIT_SEG_A_STEPS * 2) + SPIT_SEG_B_STEPS + SPIT_SEG_C_STEPS;  // 50

uint16_t spitFreq[MAX_SPIT_STEPS];  // 0 = silence
uint16_t spitDur[MAX_SPIT_STEPS];
int spitSeqLen = 0;         // actual length used this rep
int spitPumpFireStep = 0;   // computed fresh each rep -- index of first Segment C step

// ---------------- State machine ----------------
enum State { IDLE,
             RAMP_TO_RED,
             SNEEZE_PLAY,
             SNEEZE_GAP,
             FADE_TO_BLACK,
             FADE_IN_IDLE };
State state = IDLE;
unsigned long stateStartTime = 0;

// ramp
int rampStartR, rampStartG, rampStartB;
unsigned long nextTrillTime = 0;  // next scheduled warning trill during RAMP_TO_RED
int trillSegment = -1;            // -1 = not playing, 0 = tension rise, 1 = flutter, 2 = drop
int trillStep = 0;
unsigned long trillStepStart = 0;

// sneeze playback
int sneezeRep = 0;
int sneezeStepIdx = 0;
unsigned long stepStartTime = 0;
bool pumpFiredThisRep = false;

// pump (handled independently every loop, non-blocking)
bool pumpOn = false;
unsigned long pumpOffAt = 0;

// button debounce
int lastRawButtonState = HIGH;
unsigned long lastDebounceTime = 0;
bool buttonStable = HIGH;

// TOF debounce -- target must read continuously in-zone for tofDebounce ms
bool tofDebouncing = false;
unsigned long tofInZoneSince = 0;

// current eye color (so ramp/fade can read the live idle color)
int curR = 0, curG = 0, curB = 0;

// quiet-time cooldown -- neither trigger fires again until millis() >= cooldownUntil
unsigned long cooldownUntil = 0;



// ******************** setup() ********************
void setup() {
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SPEAKER_PIN, OUTPUT);

  pinMode(EYE_R_PIN, OUTPUT);
  pinMode(EYE_G_PIN, OUTPUT);
  pinMode(EYE_B_PIN, OUTPUT);

  Serial.begin(115200);
  Serial.println();
  Serial.println("Alien spitter ready.");

  Wire.begin(sdaPin, sclPin);
  delay(750);

  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X"));
    while (1)
      ;
  }

  stateStartTime = millis();
}



// ******************** loop ********************
void loop() {
  unsigned long now = millis();

  VL53L0X_RangingMeasurementData_t measure;
  measure.RangeMilliMeter = 0;
  measure.RangeStatus = 4;  // treat as "invalid" when we skip the sensor read below

  // Only poll the TOF sensor in states that actually use its result: IDLE
  // (waiting for a trigger) and RAMP_TO_RED (target-left-zone abort check).
  // SNEEZE_PLAY/SNEEZE_GAP/the fades never read `measure`, so skipping the
  // sensor call there is free -- and it matters a lot, because
  // lox.rangingTest() blocks for ~20-30ms. With it running every loop()
  // iteration regardless of state, loop() itself was bottlenecked to one
  // cycle per ~20-30ms, which silently ate the spit sound's short
  // (as low as 5-8ms) step durations and dragged the whole thing out slow.
  if (state == IDLE || state == RAMP_TO_RED) {
    lox.rangingTest(&measure, false);  // pass in 'true' to get debug data printout!
  }

  updateTrigger(now, measure);
  updatePump(now);

  switch (state) {
    case IDLE: runIdle(now); break;
    case RAMP_TO_RED: runRamp(now); break;
    case SNEEZE_PLAY: runSneezePlay(now); break;
    case SNEEZE_GAP: runSneezeGap(now); break;
    case FADE_TO_BLACK: runFadeToBlack(now); break;
    case FADE_IN_IDLE: runFadeInIdle(now); break;
  }
}



//  ******************** Trigger: button OR TOF proximity  ********************
void updateTrigger(unsigned long now, const VL53L0X_RangingMeasurementData_t &measure) {
  bool canTrigger = (state == IDLE) && (now >= cooldownUntil);

  // --- abort ramp if the target leaves the sneeze zone before it completes ---
  // (button-triggered ramps are left alone here since they have no zone to leave)
  if (state == RAMP_TO_RED) {
    bool inZone = (measure.RangeMilliMeter > sneezeZoneMin) &&
                  (measure.RangeMilliMeter < sneezeZoneMax) &&
                  (measure.RangeStatus != 4);
    if (!inZone) {
      Serial.println("Target left sneeze zone -> abort ramp, back to idle");
      noTone(SPEAKER_PIN);  // cut off the trill if it was mid-play
      trillSegment = -1;    // cancel the in-progress trill
      state = IDLE;
      stateStartTime = now;
    }
  }

  // --- button, debounced ---
  int raw = digitalRead(BUTTON_PIN);

  if (raw != lastRawButtonState) {
    lastDebounceTime = now;
    lastRawButtonState = raw;
  }

  bool buttonJustPressed = false;
  if (now - lastDebounceTime > DEBOUNCE_MS) {
    if (raw != buttonStable) {
      buttonStable = raw;
      if (buttonStable == LOW) {
        buttonJustPressed = true;
      }
    }
  }

  // Button no longer triggers the sneeze sequence -- it just runs the pump
  // for PRIME_PUMP_MS to push fluid through the ~15ft of tubing between the
  // pump and the prop. Only allowed while IDLE and not already running the
  // pump, so it can't stomp on a sneeze pulse or restart itself mid-prime.
  if (buttonJustPressed && state == IDLE && !pumpOn) {
    Serial.println("Button pressed -> priming pump (5s)");
    firePumpFor(now, PRIME_PUMP_MS);
    return;
  }

  // --- TOF proximity, debounced ---
  // RangeStatus 4 means phase failure / bad reading, so it's excluded here
  // even though it isn't a trigger to begin with. The target must read
  // in-zone continuously for tofDebounce ms before this actually fires,
  // to filter out single-frame noise blips that would otherwise start a
  // ramp (chirp) and immediately abort it.
  bool tofInZone = (measure.RangeMilliMeter > sneezeZoneMin) &&
                    (measure.RangeMilliMeter < sneezeZoneMax) &&
                    (measure.RangeStatus != 4);

  if (tofInZone && canTrigger) {
    if (!tofDebouncing) {
      tofDebouncing = true;
      tofInZoneSince = now;
    } else if (now - tofInZoneSince >= tofDebounce) {
      Serial.print("TOF proximity trigger ");
      Serial.print(measure.RangeMilliMeter);
      Serial.println(" mm");
      beginRamp(now);
      tofDebouncing = false;
    }
  } else {
    tofDebouncing = false;
  }
}


// ******************** Pump (independent timer)  ********************
void firePumpFor(unsigned long now, unsigned long durationMs) {
  digitalWrite(PUMP_PIN, HIGH);
  pumpOn = true;
  pumpOffAt = now + durationMs;
}

void firePumpPulse(unsigned long now) {
  firePumpFor(now, PUMP_PULSE_MS);
}

void updatePump(unsigned long now) {
  if (pumpOn && now >= pumpOffAt) {
    digitalWrite(PUMP_PIN, LOW);
    pumpOn = false;
  }
}


//  ******************** Eyes  ********************
void setEyeColor(int r, int g, int b) {
  curR = r;
  curG = g;
  curB = b;
  analogWrite(EYE_R_PIN, r);
  analogWrite(EYE_G_PIN, g);
  analogWrite(EYE_B_PIN, b);
}

int lerp(int a, int b, float t) {
  return a + (int)((b - a) * t);
}


//  ******************** IDLE: slow, smooth, never-off blue<->green breathing  ********************
void computeIdleColor(unsigned long elapsed, int &g, int &b) {
  float bPhase = (float)(elapsed % BREATH_PERIOD_MS) / (float)BREATH_PERIOD_MS;
  float bWave = (sinf(bPhase * 2.0 * PI) + 1.0) / 2.0;  // 0..1
  int level = IDLE_MIN_BRIGHTNESS + (int)(bWave * (IDLE_MAX_BRIGHTNESS - IDLE_MIN_BRIGHTNESS));

  float cPhase = (float)(elapsed % COLOR_PERIOD_MS) / (float)COLOR_PERIOD_MS;
  float cWave = (sinf(cPhase * 2.0 * PI) + 1.0) / 2.0;  // 0..1, 0=blue 1=green

  g = (int)(level * cWave);
  b = (int)(level * (1.0 - cWave));
}

void runIdle(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;
  int g, b;
  computeIdleColor(elapsed, g, b);
  setEyeColor(0, g, b);
}

//  ******************** Warning Trill  ********************
int jitter(int base, int amount) {
  return base + random(-amount, amount);
}

const int TRILL_A_STEPS = 15;
const int TRILL_A_STEP_MS = 10;
const int TRILL_B_STEPS = 12;
const int TRILL_B_STEP_MS = 20;
const int TRILL_C_STEPS = 15;
const int TRILL_C_STEP_MS = 10;

int trillStepFreq(int segment, int step) {
  switch (segment) {
    case 0: return jitter(420 + (step * 10), 20);
    case 1: return jitter((step % 2 == 0) ? 540 : 610, 15);
    case 2: return jitter(500 - (step * 12), 10);
  }
  return 0;
}

int trillStepDur(int segment) {
  switch (segment) {
    case 0: return TRILL_A_STEP_MS;
    case 1: return TRILL_B_STEP_MS;
    case 2: return TRILL_C_STEP_MS;
  }
  return 0;
}

int trillSegmentSteps(int segment) {
  switch (segment) {
    case 0: return TRILL_A_STEPS;
    case 1: return TRILL_B_STEPS;
    case 2: return TRILL_C_STEPS;
  }
  return 0;
}

void startTrill(unsigned long now) {
  trillSegment = 0;
  trillStep = 0;
  trillStepStart = now;
  tone(SPEAKER_PIN, trillStepFreq(0, 0), trillStepDur(0));
}

void updateTrill(unsigned long now) {
  if (trillSegment < 0) return;

  if (now - trillStepStart >= (unsigned long)trillStepDur(trillSegment)) {
    trillStep++;
    if (trillStep >= trillSegmentSteps(trillSegment)) {
      trillSegment++;
      trillStep = 0;
      if (trillSegment > 2) {
        noTone(SPEAKER_PIN);
        trillSegment = -1;
        return;
      }
    }
    trillStepStart = now;
    tone(SPEAKER_PIN, trillStepFreq(trillSegment, trillStep), trillStepDur(trillSegment));
  }
}

// ---------------- RAMP_TO_RED ----------------
void beginRamp(unsigned long now) {
  rampStartR = curR;
  rampStartG = curG;
  rampStartB = curB;
  state = RAMP_TO_RED;
  stateStartTime = now;
  nextTrillTime = now;
  trillSegment = -1;
}

void runRamp(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;
  float t = (float)elapsed / (float)RAMP_MS;
  if (t >= 1.0) t = 1.0;

  int r = lerp(rampStartR, 1023, t);
  int g = lerp(rampStartG, 0, t);
  int b = lerp(rampStartB, 0, t);
  setEyeColor(r, g, b);

  updateTrill(now);

  if (now >= nextTrillTime) {
    startTrill(now);
    float easedT = pow(t, CHIRP_EASE_POWER);
    unsigned long interval = CHIRP_INTERVAL_START - (unsigned long)((CHIRP_INTERVAL_START - CHIRP_INTERVAL_END) * easedT);
    nextTrillTime = now + interval;
  }

  if (t >= 1.0) {
    beginSneezeRep(now);
  }
}

// ---------------- SNEEZE_PLAY / SNEEZE_GAP ----------------
void beginSneezeRep(unsigned long now) {
  sneezeRep = 0;
  startSneezeSequence(now);
}

void generateSpitSequence() {
  int idx = 0;

  // Segment A: irregular high-frequency charge (tone + variable gap)
  for (int i = 0; i < SPIT_SEG_A_STEPS; i++) {
    int freq = jitter(900 + random(0, 300), 40);
    int totalDur = random(8, 14);  // 8..13, matches original delay()

    spitFreq[idx] = freq;
    spitDur[idx] = 8;  // tone always plays exactly 8ms, like the original
    idx++;

    int gap = totalDur - 8;
    if (gap > 0) {
      spitFreq[idx] = 0;  // silence marker
      spitDur[idx] = gap;
      idx++;
    }
  }

  // Segment B: chaotic dual-tone beating -- no gap, back-to-back tones
  for (int i = 0; i < SPIT_SEG_B_STEPS; i++) {
    int base = jitter(600 + random(-80, 80), 25);
    int harmonic = jitter(base + random(40, 120), 20);
    spitFreq[idx] = (i % 2 == 0) ? base : harmonic;
    spitDur[idx] = 10;
    idx++;
  }

  spitPumpFireStep = idx;  // pump fires at the start of Segment C

  // Segment C: low-frequency expulsion -- no gap
  for (int i = 0; i < SPIT_SEG_C_STEPS; i++) {
    spitFreq[idx] = jitter(350 - (i * 10), 30);
    spitDur[idx] = 10;
    idx++;
  }

  spitSeqLen = idx;
}

void startSneezeSequence(unsigned long now) {
  generateSpitSequence();  // fresh randomized spit sound each rep
  sneezeStepIdx = 0;
  pumpFiredThisRep = false;
  stepStartTime = now;
  state = SNEEZE_PLAY;
  if (spitFreq[0] == 0) noTone(SPEAKER_PIN); else tone(SPEAKER_PIN, spitFreq[0]);
}

void runSneezePlay(unsigned long now) {
  setEyeColor(1023, 0, 0);  // solid red while sneezing

  if (sneezeStepIdx == spitPumpFireStep && !pumpFiredThisRep) {
    firePumpPulse(now);
    pumpFiredThisRep = true;
  }

  if (now - stepStartTime >= spitDur[sneezeStepIdx]) {
    sneezeStepIdx++;
    if (sneezeStepIdx >= spitSeqLen) {
      noTone(SPEAKER_PIN);
      sneezeRep++;
      if (sneezeRep >= SNEEZE_REPS) {
        beginFadeToBlack(now);
      } else {
        state = SNEEZE_GAP;
        stateStartTime = now;
      }
    } else {
      if (spitFreq[sneezeStepIdx] == 0) noTone(SPEAKER_PIN); else tone(SPEAKER_PIN, spitFreq[sneezeStepIdx]);
      stepStartTime = now;
    }
  }
}

void runSneezeGap(unsigned long now) {
  setEyeColor(1023, 0, 0);  // stay red between reps
  if (now - stateStartTime >= SNEEZE_GAP_MS) {
    startSneezeSequence(now);
  }
}

// ---------------- FADE_TO_BLACK: red -> black over 1s ----------------
void beginFadeToBlack(unsigned long now) {
  state = FADE_TO_BLACK;
  stateStartTime = now;
}

void runFadeToBlack(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;
  float t = (float)elapsed / (float)FADE_OUT_MS;
  if (t >= 1.0) t = 1.0;

  int r = lerp(1023, 0, t);
  setEyeColor(r, 0, 0);

  if (t >= 1.0) {
    beginFadeInIdle(now);
  }
}

// ---------------- FADE_IN_IDLE: black -> idle breathing over 1s ----------------
void beginFadeInIdle(unsigned long now) {
  state = FADE_IN_IDLE;
  stateStartTime = now;
}

void runFadeInIdle(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;
  float t = (float)elapsed / (float)FADE_IN_MS;
  if (t >= 1.0) t = 1.0;

  int g, b;
  computeIdleColor(elapsed, g, b);
  g = (int)(g * t);
  b = (int)(b * t);
  setEyeColor(0, g, b);

  if (elapsed >= FADE_IN_MS) {
    state = IDLE;
    cooldownUntil = now + QUIET_MS;
  }
}