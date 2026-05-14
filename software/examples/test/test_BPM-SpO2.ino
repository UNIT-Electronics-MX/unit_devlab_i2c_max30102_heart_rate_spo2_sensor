#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

MAX30105 sensor;

#define SDA_PIN 1
#define SCL_PIN 6

// ---------- BPM ----------
const byte RATE_SIZE = 8;
float rates[RATE_SIZE];
byte rateSpot = 0;
byte validRates = 0;

float bpmInst = 0;
float bpmAvg = 0;

unsigned long lastBeatTime = 0;
bool wasAbove = false;

// Filtro de señal
float irDC = 0;
float pulseSignal = 0;
float threshold = 80;

// ---------- SpO2 ----------
#define SPO2_SAMPLES 100

uint32_t irBuffer[SPO2_SAMPLES];
uint32_t redBuffer[SPO2_SAMPLES];

byte spo2Index = 0;

int32_t spo2;
int8_t validSPO2;
int32_t dummyHR;
int8_t dummyValidHR;

float spo2Avg = 0;
int lastValidSpO2 = 0;
bool spo2Ready = false;

// ---------- Configuración de señal ----------
const long FINGER_THRESHOLD = 8000;
const long SIGNAL_LOW = 25000;
const long SIGNAL_GOOD_MAX = 110000;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("===============================================");
  Serial.println(" MAX30102 - BPM + SpO2 Experimental");
  Serial.println("===============================================");
  Serial.println("Nota: No es equipo medico. Solo uso experimental.");
  Serial.println();

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!sensor.begin(Wire, I2C_SPEED_FAST))
  {
    Serial.println("MAX30102 no detectado");
    while (1);
  }

  sensor.setup(
    50,     // Brillo base
    4,      // Promedio
    2,      // RED + IR
    100,    // Sample rate 100 Hz
    411,    // Pulse width
    16384   // ADC amplio
  );

  sensor.setPulseAmplitudeRed(0x24);
  sensor.setPulseAmplitudeIR(0x24);
  sensor.setPulseAmplitudeGreen(0);

  Serial.println("Coloca el dedo suavemente sobre el sensor.");
  Serial.println("Evita presionar fuerte y espera 10 a 20 segundos.");
  Serial.println();
}

void loop()
{
  long irValue = sensor.getIR();
  long redValue = sensor.getRed();

  // ---------- Detectar dedo ----------
  if (irValue < FINGER_THRESHOLD)
  {
    Serial.println("No se detecta dedo");

    bpmInst = 0;
    bpmAvg = 0;
    validRates = 0;
    rateSpot = 0;

    lastValidSpO2 = 0;
    spo2Avg = 0;
    spo2Ready = false;
    spo2Index = 0;

    irDC = 0;
    threshold = 80;
    wasAbove = false;

    delay(300);
    return;
  }

  // ---------- Calidad de señal ----------
  String calidad;

  if (irValue < SIGNAL_LOW)
    calidad = "Baja";
  else if (irValue < SIGNAL_GOOD_MAX)
    calidad = "Buena";
  else
    calidad = "Saturada";

  bool signalUsableForSpO2 = (irValue >= SIGNAL_LOW && irValue < SIGNAL_GOOD_MAX);

  // ---------- Filtro DC / AC ----------
  if (irDC == 0) irDC = irValue;

  irDC = (irDC * 0.95) + (irValue * 0.05);
  pulseSignal = irValue - irDC;

  float absSignal = fabs(pulseSignal);
  threshold = (threshold * 0.95) + (absSignal * 0.05);

  bool isAbove = pulseSignal > threshold * 0.6;

  // ---------- Detección de latido ----------
  if (isAbove && !wasAbove)
  {
    unsigned long now = millis();
    unsigned long delta = now - lastBeatTime;

    if (delta > 300 && delta < 1500)
    {
      bpmInst = 60000.0 / delta;

      if (bpmInst >= 45 && bpmInst <= 160)
      {
        rates[rateSpot++] = bpmInst;
        rateSpot %= RATE_SIZE;

        if (validRates < RATE_SIZE) validRates++;

        float sum = 0;
        for (byte i = 0; i < validRates; i++)
        {
          sum += rates[i];
        }

        bpmAvg = sum / validRates;
      }
    }

    lastBeatTime = now;
  }

  wasAbove = isAbove;

  if (millis() - lastBeatTime > 3000)
  {
    bpmInst = 0;
  }

  // ---------- SpO2 ----------
  irBuffer[spo2Index] = irValue;
  redBuffer[spo2Index] = redValue;
  spo2Index++;

  if (spo2Index >= SPO2_SAMPLES)
  {
    maxim_heart_rate_and_oxygen_saturation(
      irBuffer,
      SPO2_SAMPLES,
      redBuffer,
      &spo2,
      &validSPO2,
      &dummyHR,
      &dummyValidHR
    );

    if (validSPO2 && spo2 >= 60 && spo2 <= 100 && signalUsableForSpO2)
    {
      lastValidSpO2 = spo2;

      if (!spo2Ready)
      {
        spo2Avg = spo2;
        spo2Ready = true;
      }
      else
      {
        spo2Avg = (spo2Avg * 0.85) + (spo2 * 0.15);
      }
    }

    spo2Index = 0;
  }

  // ---------- Estado SpO2 ----------
  String estadoSpO2 = "OK";

  if (spo2Ready && spo2Avg < 90)
  {
    estadoSpO2 = "BAJO/Revisar";
  }
  else if (!signalUsableForSpO2)
  {
    estadoSpO2 = "Senal no ideal";
  }

  // ---------- Imprimir ----------
  char line[220];

  if (spo2Ready)
  {
    snprintf(line, sizeof(line),
      "IR:%6ld | RED:%6ld | AC:%8.1f | BPM inst:%6.1f | BPM prom:%6.1f | SpO2:%5.1f%% | Estado:%-13s | Senal:%s",
      irValue,
      redValue,
      pulseSignal,
      bpmInst,
      bpmAvg,
      spo2Avg,
      estadoSpO2.c_str(),
      calidad.c_str()
    );
  }
  else
  {
    snprintf(line, sizeof(line),
      "IR:%6ld | RED:%6ld | AC:%8.1f | BPM inst:%6.1f | BPM prom:%6.1f | SpO2:  ---  | Estado:%-13s | Senal:%s",
      irValue,
      redValue,
      pulseSignal,
      bpmInst,
      bpmAvg,
      estadoSpO2.c_str(),
      calidad.c_str()
    );
  }

  Serial.println(line);

  delay(200);
}