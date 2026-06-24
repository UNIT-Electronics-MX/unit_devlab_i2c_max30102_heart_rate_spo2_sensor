/*
 * MAX30102 - INT pin data-ready test
 * Module: UE0133 MAX30102 Heart Rate and SpO2 Sensor
 * Board : Pulsar C6 (ESP32-C6)
 *
 * Connections:
 *   SDA -> GPIO6
 *   SCL -> GPIO7
 *   INT -> GPIO4 (D7 on Pulsar C6)
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

  pinMode(INT_PIN, INPUT);
  Serial.print(F("INT idle level: "));
  Serial.println(digitalRead(INT_PIN) == HIGH ? F("HIGH") : F("LOW"));

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Sensor not found"));
    while (1);
  }

  Serial.println(F("Sensor found"));

  // Minimal configuration to generate data-ready interrupts.
  particleSensor.setup(0x1F, 1, 2, 100, 69, 4096);

  // Clear pending interrupts before registering the ISR.
  particleSensor.getINT1();
  particleSensor.getINT2();
  delay(10);

  particleSensor.enableDATARDY();
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onInterrupt, FALLING);

  Serial.println(F("Waiting for interrupts..."));
}

void loop() {
  if (interruptDetected) {
    interruptDetected = false;

    uint8_t flags = particleSensor.getINT1();

    Serial.print(F("INT detected - flags: 0b"));
    for (int i = 7; i >= 0; i--) {
      Serial.print((flags >> i) & 1);
    }

    if (flags & 0x40) {
      Serial.print(F(" -> PPG_RDY"));
    }

    if (flags & 0x80) {
      Serial.print(F(" -> A_FULL"));
    }

    Serial.println();
  }
}
