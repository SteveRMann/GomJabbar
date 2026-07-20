// ============================================================
// Alien Spitter Prop - Wemos D1 Mini (ESP8266)
// ============================================================
// Sequence on button press OR TOF proximity trigger:
//   1. Eyes ramp blue-green(idle) -> red over 1s, with an
//      accelerating warning trill building tension
//   2. 3x { sneeze tone burst + pump pulse, simultaneously }
//   3. Eyes fade red -> black over 1s, then fade black -> idle breathing over 1s
//   4. Neither trigger can fire again until QUIET_MS has passed
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


// ---------------- Tunable ----------------
const unsigned long BREATH_PERIOD_MS = 7000;  // one idle brightness breathe cycle
const unsigned long COLOR_PERIOD_MS = 16000;  // one blue<->green color drift cycle (deliberately not a clean multiple of BREATH_PERIOD_MS so the two never lock into a repeating pattern)
const int IDLE_MAX_BRIGHTNESS = 700;          // of 1023, idle eye ceiling
const int IDLE_MIN_BRIGHTNESS = 180;          // of 1023, idle eye floor -- never fully off

const unsigned long RAMP_MS = 5000;           // idle-color -> red ramp

// ---------------- Warning trill (ramp charge-up sound) ----------------
// Plays during RAMP_TO_RED: an organic, jittered trill (tension rise ->
// flutter -> drop) that repeats faster and faster as the ramp progresses,
// building tension into the sneeze burst. This is the same interval/easing
// schedule the old chirp used -- only the sound itself changed.
const unsigned long CHIRP_INTERVAL_START = 450;  // ms between trills at ramp start (slow)
const unsigned long CHIRP_INTERVAL_END = 60;     // ms between trills at ramp end (fast)
const float CHIRP_EASE_POWER = 2.2;              // >1 = stays slow longer, then rushes near the end

const int SNEEZE_REPS = 3;
const unsigned long SNEEZE_GAP_MS = 400;  // pause between reps
const unsigned long PUMP_PULSE_MS = 50;   // pump ON time per pulse
const unsigned long FADE_OUT_MS = 1000;   // red -> black
const unsigned long FADE_IN_MS = 1000;    // black -> idle breathing

const int sneezeZoneMin = 200;           // Minimum distance (mm) to TOF sensor. Anything greater will trigger a sneeze
const int sneezeZoneMax = 8000;          // Maximum distance to trigger a sneeze. Anything greater is ignored.
const unsigned long tofDebounce = 500;   // ms the target must be continuously in-zone before a TOF trigger fires
const unsigned long QUIET_MS = 2000;  // cooldown after a sequence ends before button OR TOF can trigger the next one

const unsigned long DEBOUNCE_MS = 30;

// ---------------- Sneeze tone sequence ----------------
// Invented alien "sneeze": rising charge-up whine, jagged burst,
// descending wet honk tail. Tune freely by ear.
struct ToneStep {
  uint16_t freq;
  uint16_t dur;
};

const ToneStep sneezeSeq[] = {
  // charge-up whine (rising)
  { 400, 60 },
  { 500, 55 },
  { 650, 50 },
  { 800, 45 },
  { 1000, 40 },
  { 1300, 35 },
  // crackling burst  <-- pump fires when this segment starts
  { 2400, 25 },
  { 3400, 18 },
  { 1600, 15 },
  { 2800, 18 },
  { 1100, 15 },
  { 2000, 15 },
  // wet descending tail
  { 350, 70 },
  { 220, 90 },
  { 140, 120 }
};
const int SNEEZE_SEQ_LEN = sizeof(sneezeSeq) / sizeof(sneezeSeq[0]);
const int PUMP_FIRE_STEP = 6;  // index of first "burst" step above

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
  lox.rangingTest(&measure, false);  // pass in 'true' to get debug data printout!

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

  if (buttonJustPressed && canTrigger) {
    Serial.println("Button pressed -> alert sequence");
    beginRamp(now);
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
void firePumpPulse(unsigned long now) {
  digitalWrite(PUMP_PIN, HIGH);
  pumpOn = true;
  pumpOffAt = now + PUMP_PULSE_MS;
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
// Pure function of "elapsed" so both steady idle and the fade-in can share it.
void computeIdleColor(unsigned long elapsed, int &g, int &b) {
  // brightness: continuous sine, floored so it never reaches 0
  float bPhase = (float)(elapsed % BREATH_PERIOD_MS) / (float)BREATH_PERIOD_MS;
  float bWave = (sinf(bPhase * 2.0 * PI) + 1.0) / 2.0;  // 0..1
  int level = IDLE_MIN_BRIGHTNESS + (int)(bWave * (IDLE_MAX_BRIGHTNESS - IDLE_MIN_BRIGHTNESS));

  // color: separate, slower sine blends continuously between blue and green
  // (no hard switch -- always some mix of both)
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

//  ******************** Warning Trill (organic, non-blocking port of the Water Watcher test sketch)  ********************
// Same three segments as the original -- tension rise, flutter, drop & relax
// -- each built from short randomly-jittered tone steps. The original used
// delay() between steps; here each step is timed against millis() so it
// never blocks the loop() (TOF polling, pump timer, trigger checks, etc.
// keep running normally while a trill plays).

int jitter(int base, int amount) {
  return base + random(-amount, amount);
}

const int TRILL_A_STEPS = 15;
const int TRILL_A_STEP_MS = 10;  // Segment A: tension rise, 15 x 10ms = 150ms
const int TRILL_B_STEPS = 12;
const int TRILL_B_STEP_MS = 20;  // Segment B: flutter, 12 x 20ms = 240ms
const int TRILL_C_STEPS = 15;
const int TRILL_C_STEP_MS = 10;  // Segment C: drop & relax, 15 x 10ms = 150ms

int trillStepFreq(int segment, int step) {
  switch (segment) {
    case 0: return jitter(420 + (step * 10), 20);           // rising + wobble
    case 1: return jitter((step % 2 == 0) ? 540 : 610, 15);  // alternating flutter
    case 2: return jitter(500 - (step * 12), 10);            // dropping + wobble
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
  if (trillSegment < 0) return;  // not playing

  if (now - trillStepStart >= (unsigned long)trillStepDur(trillSegment)) {
    trillStep++;
    if (trillStep >= trillSegmentSteps(trillSegment)) {
      trillSegment++;
      trillStep = 0;
      if (trillSegment > 2) {
        noTone(SPEAKER_PIN);
        trillSegment = -1;  // trill finished
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
  nextTrillTime = now;  // fire the first trill right away
  trillSegment = -1;    // ensure no stale trill carries over
}

void runRamp(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;
  float t = (float)elapsed / (float)RAMP_MS;
  if (t >= 1.0) t = 1.0;

  int r = lerp(rampStartR, 1023, t);
  int g = lerp(rampStartG, 0, t);
  int b = lerp(rampStartB, 0, t);
  setEyeColor(r, g, b);

  // advance any warning trill currently mid-playback
  updateTrill(now);

  // Accelerating trill: the gap between trills shrinks from
  // CHIRP_INTERVAL_START down to CHIRP_INTERVAL_END as t goes 0->1.
  // CHIRP_EASE_POWER > 1 keeps the gaps wide early on and rushes them
  // together near the end of the ramp, right before the sneeze fires.
  // Note: a full trill takes ~540ms, so once the interval shrinks below
  // that (it ends at 60ms), a new trill retriggers mid-playback and cuts
  // the previous one off -- that clipped, urgent stutter near the end of
  // the ramp is expected, not a bug.
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

void startSneezeSequence(unsigned long now) {
  sneezeStepIdx = 0;
  pumpFiredThisRep = false;
  stepStartTime = now;
  state = SNEEZE_PLAY;
  tone(SPEAKER_PIN, sneezeSeq[0].freq);
}

void runSneezePlay(unsigned long now) {
  setEyeColor(1023, 0, 0);  // solid red while sneezing

  if (sneezeStepIdx == PUMP_FIRE_STEP && !pumpFiredThisRep) {
    firePumpPulse(now);
    pumpFiredThisRep = true;
  }

  unsigned long stepDur = sneezeSeq[sneezeStepIdx].dur;
  if (now - stepStartTime >= stepDur) {
    sneezeStepIdx++;
    if (sneezeStepIdx >= SNEEZE_SEQ_LEN) {
      noTone(SPEAKER_PIN);
      sneezeRep++;
      if (sneezeRep >= SNEEZE_REPS) {
        beginFadeToBlack(now);
      } else {
        state = SNEEZE_GAP;
        stateStartTime = now;
      }
    } else {
      tone(SPEAKER_PIN, sneezeSeq[sneezeStepIdx].freq);
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
// Idle's own animation runs from t=0 here (via computeIdleColor), scaled by
// fade progress, so it arrives already in motion rather than snapping on.
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
    // Switch to steady idle WITHOUT resetting stateStartTime, so the
    // breathing/color-drift animation continues seamlessly from here
    // instead of jumping back to its own t=0.
    state = IDLE;
    cooldownUntil = now + QUIET_MS;  // neither trigger fires again until this passes
  }
}
