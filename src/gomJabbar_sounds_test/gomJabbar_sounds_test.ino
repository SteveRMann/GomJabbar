
// Water Watcher sounds

const int SPEAKER_PIN = D8;  // tone() output


// small helper to add organic wobble
int jitter(int base, int amount) {
  return base + random(-amount, amount);
}


//-------------------------------------------------
// Warning trill
void waterWatcherTrill() {
  // Segment A: Tension Rise (150 ms total)
  for (int i = 0; i < 15; i++) {
    int freq = jitter(420 + (i * 10), 20);  // rising + wobble
    tone(SPEAKER_PIN, freq, 10);
    delay(10);
  }

  // Segment B: Flutter (250 ms total)
  for (int i = 0; i < 12; i++) {
    int freq = jitter((i % 2 == 0) ? 540 : 610, 15);  // alternating flutter
    tone(SPEAKER_PIN, freq, 20);
    delay(20);
  }

  // Segment C: Drop & Relax (150 ms total)
  for (int i = 0; i < 15; i++) {
    int freq = jitter(500 - (i * 12), 10);  // dropping + wobble
    tone(SPEAKER_PIN, freq, 10);
    delay(10);
  }

  noTone(SPEAKER_PIN);
}


//-------------------------------------------------
// Spit sound
void waterWatcherSpit() {
  // Segment A: Pressure Pop (60 ms total)
  for (int i = 0; i < 6; i++) {
    int freq = jitter(1000, 80);  // sharp, high burst
    tone(SPEAKER_PIN, freq, 10);
    delay(10);
  }

  // Segment B: Descending Rasp (180 ms total)
  for (int i = 0; i < 18; i++) {
    int freq = jitter(1000 - (i * 40), 25);  // fast downward sweep
    tone(SPEAKER_PIN, freq, 10);
    delay(10);
  }

  // Segment C: Wet Flutter (100 ms total)
  for (int i = 0; i < 5; i++) {
    int freq = jitter((i % 2 == 0) ? 420 : 360, 20);  // alternating flutter
    tone(SPEAKER_PIN, freq, 20);
    delay(20);
  }

  noTone(SPEAKER_PIN);
}



//-------------------------------------------------
void waterWatcherAlienSpit() {

  // Segment A: Irregular High-Frequency Charge (100 ms)
  for (int i = 0; i < 10; i++) {
    int freq = jitter(900 + random(0, 300), 40);  // unpredictable spikes
    tone(SPEAKER_PIN, freq, 8);
    delay(random(8, 14));  // uneven timing
  }

  // Segment B: Chaotic Dual-Tone Beating (180 ms)
  for (int i = 0; i < 18; i++) {
    int base = jitter(600 + random(-80, 80), 25);
    int harmonic = jitter(base + random(40, 120), 20);  // second tone
    tone(SPEAKER_PIN, (i % 2 == 0) ? base : harmonic, 10);
    delay(10);
  }

  // Segment C: Low-Frequency Expulsion (120 ms)
  for (int i = 0; i < 12; i++) {
    int freq = jitter(350 - (i * 10), 30);  // unstable downward push
    tone(SPEAKER_PIN, freq, 10);
    delay(10);
  }

  noTone(SPEAKER_PIN);
}



void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("gomJabbar_Sound_test");
}


void loop() {
  Serial.println("Trill");
  waterWatcherTrill();
  delay(200);
  waterWatcherAlienSpit();
  //  waterWatcherSpit();
  delay(2000);
}