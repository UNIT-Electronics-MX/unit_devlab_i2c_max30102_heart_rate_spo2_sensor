/*
 * MAX30102 - proximity interrupt test
 * Module: UE0133 MAX30102 Heart Rate and SpO2 Sensor
 * Board : Pulsar C6 (ESP32-C6)
 *
 * Connections:
 *   SDA -> GPIO6
 *   SCL -> GPIO7
 *   INT -> GPIO4 (D7 on Pulsar C6)
 *
 * Expected behavior:
 *   No finger      -> IR LED uses low-current proximity scan.
 *   Finger nearby  -> PROX_INT triggers and the serial monitor reports it.
 *
 * Serial monitor:
 *   115200 baud
 */

#include <Wire.h>
#include "MAX30105.h"

#define SDA_PIN 6
#define SCL_PIN 7
#define INT_PIN 4

MAX30105 particleSensor;

volatile bool interruptDetected = false;

void IRAM_ATTR onInterrupt() {
  interruptDetected = true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println(F("MAX30102 - proximity interrupt test"));

  pinMode(INT_PIN, INPUT);
  Serial.print(F("INT idle level: "));
  Serial.println(digitalRead(INT_PIN) == HIGH ? F("HIGH") : F("LOW"));

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Sensor not found"));
    while (1);
  }

  Serial.println(F("Sensor found"));

  particleSensor.setup(
    0x1F,   // powerLevel: normal LED current after proximity detection
    1,      // sampleAvg: no averaging
    2,      // ledMode: Red + IR
    100,    // sampleRate: 100 sps
    69,     // pulseWidth: 69 us
    4096    // adcRange: 4096 nA
  );

  // Proximity scan current. 0x1F is enough for finger detection near the IC.
  particleSensor.setPulseAmplitudeProximity(0x1F);

  // Lower values are more sensitive. 0x0F is a practical middle point.
  particleSensor.setProximityThreshold(0x0F);

  // Clear pending interrupts before registering the ISR.
  particleSensor.getINT1();
  particleSensor.getINT2();
  delay(10);

  particleSensor.enablePROXINT();
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onInterrupt, FALLING);

  Serial.println(F("Waiting for finger..."));
}

void loop() {
  if (interruptDetected) {
    interruptDetected = false;

    uint8_t flags = particleSensor.getINT1();

    if (flags & 0x10) {
      Serial.println(F("Finger detected - PROX_INT triggered"));
      Serial.println(F("The IC switched to normal SpO2/HR mode"));

      delay(2000);
      resetProximityMode();
      Serial.println(F("Waiting for next finger..."));
    }
  }
}

void resetProximityMode() {
  particleSensor.softReset();
  delay(100);
  particleSensor.setup(0x1F, 1, 2, 100, 69, 4096);
  particleSensor.setPulseAmplitudeProximity(0x1F);
  particleSensor.setProximityThreshold(0x0F);
  particleSensor.getINT1();
  particleSensor.getINT2();
  particleSensor.enablePROXINT();
}
