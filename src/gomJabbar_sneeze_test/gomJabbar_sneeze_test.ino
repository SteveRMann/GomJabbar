// gomJabbar.ino
// Alien Spitter - pump pulse test
// Wemos D1 Mini (ESP8266)

const int PUMP_PIN   = D3;  // MOSFET gate signal, active HIGH
const int BUTTON_PIN = D4;  // Button to GND, using internal pullup

const int PULSE_ON_MS  = 50;   // pump on time
const int PULSE_OFF_MS = 500;  // pump off time between pulses
const int NUM_PULSES   = 3;    // number of spits per trigger

const int DEBOUNCE_MS = 30;


// ==================== setup ====================
void setup() {
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  Serial.println();
  Serial.println("Alien spitter ready.");
}


// ==================== loop ====================
void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) == LOW) {
      spit();

      // wait for release so one press = one spit sequence
      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(10);
      }
    }
  }
}


// ==================== spit ====================
void spit() {
  Serial.println("Spit triggered!");
  for (int i = 0; i < NUM_PULSES; i++) {
    digitalWrite(PUMP_PIN, HIGH);
    delay(PULSE_ON_MS);
    digitalWrite(PUMP_PIN, LOW);
    delay(PULSE_OFF_MS);
  }
}