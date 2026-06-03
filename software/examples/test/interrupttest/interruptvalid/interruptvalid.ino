/*
 * MAX30102 — Validación de interrupción por proximidad
 * Módulo UE0133 — Unit Electronics | ESP32-C6
 *
 * Conexiones:
 *   SDA → GPIO 6  |  SCL → GPIO 7  |  INT → GPIO 4
 *
 * Comportamiento esperado:
 *   Sin dedo → IR LED pulsa a baja corriente (PILOT_PA), sin datos en FIFO
 *   Acercar dedo → PROX_INT dispara → Serial muestra "DEDO DETECTADO"
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

  Serial.println(F("MAX30102 — Test Interrupción Proximidad"));

  // ── 1. Nivel en reposo ────────────────────────────────────────────────
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

  // ── 3. Configuración base ─────────────────────────────────────────────
  // El modo proximidad se activa automáticamente al entrar a SpO2/HR
  // Los LEDs Red e IR se apagan hasta que se detecte el dedo
  particleSensor.setup(
    0x1F,   // powerLevel  — corriente normal durante SpO2 (post-detección)
    1,      // sampleAvg   — sin promediado
    2,      // ledMode     — Red + IR (SpO2)
    100,    // sampleRate  — 100 sps
    69,     // pulseWidth  — 69µs, mínimo necesario
    4096    // adcRange    — 4096 nA
  );

  // ── 4. Configurar el scan de proximidad ───────────────────────────────
  // PILOT_PA: corriente del IR LED durante el scan de proximidad
  // Valor bajo = menor consumo pero menor alcance de detección
  // 0x1F = ~6.2mA es suficiente para detectar un dedo a ~1cm
  particleSensor.setPulseAmplitudeProximity(0x1F);

  // PROX_INT_THRESH: umbral de los 8 MSBs del conteo ADC
  // Valor bajo = más sensible (detecta objetos más lejanos)
  // Valor alto = menos sensible (requiere dedo muy cerca)
  // 0x01 = muy sensible → cualquier reflejo dispara la INT
  // 0x0F = umbral medio → requiere dedo a ~1-2cm
  // 0xFF = solo dispara si el ADC satura (dedo pegado al sensor)
  particleSensor.setProximityThreshold(0x0F);

  // ── 5. Limpiar INTs pendientes ANTES de registrar la ISR ─────────────
  particleSensor.getINT1();
  particleSensor.getINT2();
  delay(10);

  // ── 6. Habilitar fuente y registrar ISR ───────────────────────────────
  particleSensor.enablePROXINT();
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onInterrupt, FALLING);

  Serial.println(F("Esperando dedo..."));
}

void loop() {
  if (intDetectada) {
    intDetectada = false;

    // getINT1() lee el status Y limpia el pin INT
    uint8_t flags = particleSensor.getINT1();

    if (flags & 0x10) {
      // Bit 4 = PROX_INT
      Serial.println(F(" DEDO DETECTADO — PROX_INT disparada"));
      Serial.println(F("  El IC transicionó a modo SpO2/HR normal"));

      // Para volver a modo proximidad hay que reescribir el MODE register
      // La librería lo hace internamente si llamas setup() o setLEDMode() de nuevo
      // Aquí lo reseteamos para que pueda detectar el siguiente acercamiento
      delay(2000);  // Pausa para observar el resultado
      particleSensor.softReset();
      delay(100);
      particleSensor.setup(0x1F, 1, 2, 100, 69, 4096);
      particleSensor.setPulseAmplitudeProximity(0x1F);
      particleSensor.setProximityThreshold(0x0F);
      particleSensor.getINT1();
      particleSensor.getINT2();
      particleSensor.enablePROXINT();
      Serial.println(F("Esperando siguiente dedo..."));
    }
  }
}