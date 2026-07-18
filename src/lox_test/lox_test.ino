#define SKETCH "lox_test.ino"
#include "Adafruit_VL53L0X.h"
#include <Wire.h>


Adafruit_VL53L0X lox = Adafruit_VL53L0X();

const int sclPin = D1;
const int sdaPin = D2;

void setup() {
  Serial.begin(115200);

  // wait until serial port opens for native USB devices
  while (!Serial) {
    delay(1);
  }

  Serial.println();
  Serial.println();
  Serial.println(SKETCH);

  Wire.begin(sdaPin, sclPin);
  delay(500);

  Serial.println("Adafruit VL53L0X test");
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X"));
    while (1)
      ;
  }
}


void loop() {
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);  // pass in 'true' to get debug data printout!

  if (measure.RangeStatus != 4) {  // phase failures have incorrect data
    Serial.print("Distance (mm): ");
    Serial.println(measure.RangeMilliMeter);
  } else {
    Serial.println(" out of range ");
  }

  delay(100);
}
