/*
 * MAX30102 — Validación simple del pin INT
 * Módulo UE0133 — Unit Electronics | ESP32-C6
 *
 * Conexiones:
 *   SDA → GPIO 6  |  SCL → GPIO 7  |  INT → GPIO 4
 */

#include <Wire.h>
#include "MAX30105.h"

#define SDA_PIN  6
#define SCL_PIN  7
#define INT_PIN  D7

MAX30105 particleSensor;

volatile bool intDetectada = false;

void IRAM_ATTR onInterrupt() {
  intDetectada = true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  // ── 1. Verificar nivel en reposo ──────────────────────────────────────
  pinMode(INT_PIN, INPUT);
  Serial.print(F("INT en reposo: "));
  Serial.println(digitalRead(INT_PIN) == HIGH ? F("HIGH ✓") : F("LOW ✗"));

  // ── 2. Inicializar sensor ─────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Sensor no encontrado ✗"));
    while (1);
  }
  Serial.println(F("Sensor encontrado ✓"));

  // ── 3. Configuración mínima solo para generar interrupciones ──────────
  particleSensor.setup(0x1F, 1, 2, 100, 69, 4096);

  // ── 4. Limpiar cualquier INT pendiente antes de registrar la ISR ──────
  particleSensor.getINT1();
  particleSensor.getINT2();
  delay(10);

  // ── 5. Habilitar fuente y registrar ISR ───────────────────────────────
  particleSensor.enableDATARDY();  // INT en cada nueva muestra (~10ms a 100sps)
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onInterrupt, FALLING);

  Serial.println(F("Esperando interrupciones..."));
}

void loop() {
  if (intDetectada) {
    intDetectada = false;

    uint8_t flags = particleSensor.getINT1();  // Limpia el pin INT al leer

    Serial.print(F("INT detectada — flags: 0b"));
    for (int i = 7; i >= 0; i--) Serial.print((flags >> i) & 1);

    if (flags & 0x40) Serial.print(F(" -> PPG_RDY "));
    if (flags & 0x80) Serial.print(F(" -> A_FULL "));

    Serial.println();
  }
}
