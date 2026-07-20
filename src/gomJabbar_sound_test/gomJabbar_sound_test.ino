
// Water Watcher Organic Warning Trill
// Uses jittered frequencies to simulate biological vibration

const int SPEAKER_PIN = D8;  // tone() output


// small helper to add organic wobble
int jitter(int base, int amount) {
  return base + random(-amount, amount);
}



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


void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("gomJabbar_sSound_test");
    
}


void loop() {
  Serial.println("Trill");
    waterWatcherTrill();
    delay (1500);
    
}