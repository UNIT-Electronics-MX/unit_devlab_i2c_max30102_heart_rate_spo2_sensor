/*
 * MAX30102 - full configuration sweep
 * Module: UE0133 MAX30102 Heart Rate and SpO2 Sensor
 * Board : Pulsar C6 (ESP32-C6)
 *
 * Measures:
 *   RED raw, IR raw, BPM, SpO2, and temperature.
 *
 * Sweep coverage:
 *   LED current  : 5 values  (0x0F to 0xFF)
 *   Sample avg   : 6 values  (1, 2, 4, 8, 16, 32)
 *   Sample rate  : 8 values  (50 to 3200 sps)
 *   Pulse width  : 4 values  (69 to 411 us)
 *   ADC range    : 4 values  (2048 to 16384 nA)
 *
 * Valid SR x PW combinations follow the MAX30102 datasheet table 11.
 * Total valid combinations: 5 LED x 6 AVG x 4 ADC x 26 SR/PW = 3120.
 *
 * Connections:
 *   SDA -> GPIO6
 *   SCL -> GPIO7
 *
 * Serial monitor:
 *   115200 baud
 */

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

#define SDA_PIN 6
#define SCL_PIN 7

MAX30105 sensor;

/******************************************************************************
 * Measurement parameters
 *****************************************************************************/
#define CICLOS_WARMUP_HW     5        // Hardware ALC settling cycles
#define MUESTRAS_WARMUP_BEAT 15       // Samples used to warm up checkForBeat()
#define CICLOS_MEDICION_ADC  40       // ADC averaging and SpO2 sample window
#define UMBRAL_ESTAB_PCT     5.0f     // Stability variance threshold (%)
#define MAX_CICLOS_EXTRA     20       // Extra cycles while waiting for stability
#define VENTANA_BPM_MS       10000UL  // Fixed BPM measurement window
#define MARGEN_SATURACION    262130L  // IR >= this value means ADC saturation

/******************************************************************************
 * Sensor config values
 *****************************************************************************/

// LED current: I(mA) = register x 0.2
byte ledCurrents[] = {
  0x0F,   //  ~3.0 mA
  0x1F,   //  ~6.4 mA
  0x3F,   // ~12.5 mA
  0x7F,   // ~25.4 mA
  0xFF    // ~50.0 mA
};
const char* etiquetasLED[] = {
  "3.0mA", "6.2mA", "12.6mA", "25.4mA", "51.0mA"
};
#define N_LED 5

// Sample average
byte sampleAvgs[] = { 1, 2, 4, 8, 16, 32 };
const char* etiquetasAVG[] = {
  "AVG1", "AVG2", "AVG4", "AVG8", "AVG16", "AVG32"
};
#define N_AVG 6

// Sample rate
int sampleRates[] = { 50, 100, 200, 400, 800, 1000, 1600, 3200 };
const char* etiquetasSR[] = {
  "50sps", "100sps", "200sps", "400sps", "800sps", "1000sps", "1600sps", "3200sps"
};
#define N_SR 8

// Pulse width
int pulseWidths[] = { 69, 118, 215, 411 };
const char* etiquetasPW[] = {
  "69us_15b", "118us_16b", "215us_17b", "411us_18b"
};
#define N_PW 4

// ADC range
int adcRanges[] = { 2048, 4096, 8192, 16384 };
const char* etiquetasADC[] = {
  "2048nA", "4096nA", "8192nA", "16384nA"
};
#define N_ADC 4

#define LED_MODE 2  // SpO2: Red + IR

uint32_t comboActual = 0;
uint32_t combosOmitidos = 0;

/******************************************************************************
 * SR x PW validation, based on MAX30102 datasheet table 11
 *****************************************************************************/
bool combinacionValida(int sr, int pw) {
  switch (pw) {
    case  69: return true;
    case 118: return (sr <= 1600);
    case 215: return (sr <= 1000);
    case 411: return (sr <=  800);
    default:  return false;
  }
}

/******************************************************************************
 * Helpers
 *****************************************************************************/
float msPorCiclo(int sr, int avg) {
  return (1000.0f / (float)sr) * (float)avg;
}

// Finger threshold scaled by active ADC range.
// Base: 50000 counts at 2048 nA, approximately 20% of full scale.
long calcUmbralDedo(int adcRange) {
  return (long)(50000L * 2048L / (long)adcRange);
}

void descartarCiclos(int n) {
  int c = 0;
  while (c < n) {
    sensor.check();
    while (sensor.available() && c < n) {
      sensor.nextSample();
      c++;
    }
  }
}

/******************************************************************************
 * Warm up checkForBeat() without counting samples as valid beats
 *****************************************************************************/
void warmupBeat(long umbral) {
  int c = 0;
  while (c < MUESTRAS_WARMUP_BEAT) {
    sensor.check();
    while (sensor.available() && c < MUESTRAS_WARMUP_BEAT) {
      long ir = sensor.getFIFOIR();
      if (ir > umbral && ir < MARGEN_SATURACION)
        checkForBeat(ir);
      sensor.nextSample();
      c++;
    }
  }
}

/******************************************************************************
 * Check signal stability by variance
 *****************************************************************************/
bool senialEstable(int n, long umbral) {
  float buf[n];
  float suma = 0;
  int   c    = 0;

  while (c < n) {
    sensor.check();
    while (sensor.available() && c < n) {
      buf[c] = (float)sensor.getFIFOIR();
      suma  += buf[c];
      sensor.nextSample();
      c++;
    }
  }

  float prom = suma / n;
  if (prom < (float)umbral * 0.3f || prom >= (float)MARGEN_SATURACION)
    return true;

  float sumCuad = 0;
  for (int i = 0; i < n; i++) {
    float d = buf[i] - prom;
    sumCuad += d * d;
  }
  return ((sqrt(sumCuad / n) / prom) * 100.0f) <= UMBRAL_ESTAB_PCT;
}

/******************************************************************************
 * Combined measurement window: ADC average + SpO2 + BPM
 *
 * One sample buffer is used for all three calculations:
 *
 *   1. Red/IR average (DC) -> raw CSV value
 *
 *   2. SpO2:
 *      DC_red = Red sample average
 *      DC_ir  = IR sample average
 *      AC_red = Red sample standard deviation
 *      AC_ir  = IR sample standard deviation
 *      R      = (AC_red / DC_red) / (AC_ir / DC_ir)
 *      SpO2   = 110.0 - 25.0 x R
 *      Valid range: 70% to 100%
 *
 *   3. BPM with checkForBeat() in a fixed time window
 *      (capture extends until VENTANA_BPM_MS is complete)
 *
 * Requirement: finger present (irMed > umbral) and non-saturated signal.
 *****************************************************************************/
void medirTodo(
  long  &redMed,  long  &irMed,
  float &spO2Est, bool  &spO2Valido,
  float &bpmEst,  int   &beats,
  int    nADC,    long   umbral
) {
  // Buffers para SpO2
  float bufRed[nADC], bufIR[nADC];
  long  sumRed   = 0,    sumIR   = 0;
  int   cADC     = 0;

  // Variables BPM
  float sumBPM   = 0.0f;
  long  lastBeat = 0;
  beats  = 0;
  bpmEst = 0.0f;

  // Phase 1: capture nADC samples for average and SpO2.
  while (cADC < nADC) {
    sensor.check();
    while (sensor.available() && cADC < nADC) {
      long r = (long)sensor.getFIFORed();
      long i = (long)sensor.getFIFOIR();

      bufRed[cADC] = (float)r;
      bufIR [cADC] = (float)i;
      sumRed += r;
      sumIR  += i;

      // Simultaneous BPM during ADC capture.
      if (i > umbral && i < MARGEN_SATURACION) {
        if (checkForBeat(i)) {
          long ahora = millis();
          if (lastBeat > 0) {
            float bpm = 60000.0f / (float)(ahora - lastBeat);
            if (bpm >= 30.0f && bpm <= 250.0f) { sumBPM += bpm; beats++; }
          }
          lastBeat = ahora;
        }
      }
      sensor.nextSample();
      cADC++;
    }
  }

  // Promedios (DC)
  float dcRed = (float)sumRed / nADC;
  float dcIR  = (float)sumIR  / nADC;
  redMed = (long)dcRed;
  irMed  = (long)dcIR;

  // SpO2 calculation.
  // AC = window standard deviation, which represents the pulsatile component.
  spO2Valido = false;
  spO2Est    = 0.0f;

  if (irMed > umbral && irMed < MARGEN_SATURACION) {
    float sumCuadRed = 0, sumCuadIR = 0;
    for (int j = 0; j < nADC; j++) {
      float dr = bufRed[j] - dcRed;
      float di = bufIR[j]  - dcIR;
      sumCuadRed += dr * dr;
      sumCuadIR  += di * di;
    }
    float acRed = sqrt(sumCuadRed / nADC);
    float acIR  = sqrt(sumCuadIR  / nADC);

    // Avoid division by zero and signals without meaningful AC component.
    // If AC < 0.1% of DC, SpO2 is not calculable.
    if (dcRed > 0 && dcIR > 0 &&
        acRed > dcRed * 0.001f &&
        acIR  > dcIR  * 0.001f) {

      float R = (acRed / dcRed) / (acIR / dcIR);

      // Standard empirical calibration curve: SpO2 = 110 - 25 x R.
      float spo2 = 110.0f - 25.0f * R;

      // Physiological valid range: 70% to 100%.
      if (spo2 >= 70.0f && spo2 <= 100.0f) {
        spO2Est    = spo2;
        spO2Valido = true;
      }
    }
  }

  // Phase 2: extend capture until VENTANA_BPM_MS is complete.
  uint32_t tFin = millis() + VENTANA_BPM_MS;

  while (millis() < tFin) {
    sensor.check();
    while (sensor.available()) {
      long i = (long)sensor.getFIFOIR();
      if (i > umbral && i < MARGEN_SATURACION) {
        if (checkForBeat(i)) {
          long ahora = millis();
          if (lastBeat > 0) {
            float bpm = 60000.0f / (float)(ahora - lastBeat);
            if (bpm >= 30.0f && bpm <= 250.0f) { sumBPM += bpm; beats++; }
          }
          lastBeat = ahora;
        }
      }
      sensor.nextSample();
    }
  }

  bpmEst = beats >= 2 ? sumBPM / beats : 0.0f;
}

/******************************************************************************
 * CSV header
 *****************************************************************************/
void imprimirCabecera() {
  Serial.println(F("ms;#;LED_mA;AVG;SR;PW;ADC;"
                   "Red;IR;Red_norm;IR_norm;"
                   "SpO2;SpO2_valido;"
                   "BPM;Beats;"
                   "Temp_C;ms_ciclo;CiclosExtra;"
                   "Stable;Finger;Saturated"));
}

/******************************************************************************
 * Apply one configuration and measure all fields
 *****************************************************************************/
void applyAndMeasure(
  byte ledPA, byte avgVal, int sr, int pw, int adc,
  const char* lLED, const char* lAVG, const char* lSR,
  const char* lPW,  const char* lADC
) {
  sensor.setup(ledPA, avgVal, LED_MODE, sr, pw, adc);

  long  umbral  = calcUmbralDedo(adc);
  float msCiclo = msPorCiclo(sr, avgVal);

  // Warmup HW: ALC settling
  descartarCiclos(CICLOS_WARMUP_HW);

  // Verificar estabilidad
  int  extra   = 0;
  bool estable = false;
  while (!estable && extra < MAX_CICLOS_EXTRA) {
    estable = senialEstable(CICLOS_MEDICION_ADC, umbral);
    if (!estable) { extra++; descartarCiclos(2); }
  }

  // Warm up checkForBeat() before measuring.
  warmupBeat(umbral);

  // Unified measurement: ADC + SpO2 + BPM in one window.
  long  redMed = 0, irMed = 0;
  float spO2   = 0.0f;
  bool  spO2OK = false;
  float bpmEst = 0.0f;
  int   beats  = 0;

  long irCheck = 0;
  {
    // Read a quick value to detect whether a finger is present.
    int c = 0;
    while (c < 3) {
      sensor.check();
      while (sensor.available() && c < 3) {
        irCheck = (long)sensor.getFIFOIR();
        sensor.nextSample();
        c++;
      }
    }
  }

  bool dedo     = (irCheck > umbral);
  bool saturado = (irCheck >= MARGEN_SATURACION);

  if (dedo && !saturado) {
    medirTodo(redMed, irMed, spO2, spO2OK,
              bpmEst, beats,
              CICLOS_MEDICION_ADC, umbral);
  } else {
    // No finger: take only a quick average, without BPM or SpO2.
    int c = 0;
    long sR = 0, sI = 0;
    while (c < CICLOS_MEDICION_ADC) {
      sensor.check();
      while (sensor.available() && c < CICLOS_MEDICION_ADC) {
        sR += sensor.getFIFORed();
        sI += sensor.getFIFOIR();
        sensor.nextSample();
        c++;
      }
    }
    redMed = sR / CICLOS_MEDICION_ADC;
    irMed  = sI / CICLOS_MEDICION_ADC;
  }

  float temp    = sensor.readTemperature();
  float redNorm = redMed / 262143.0f;
  float irNorm  = irMed  / 262143.0f;

  // Recalculate finger/saturation state with the real average.
  dedo     = (irMed > umbral);
  saturado = (irMed >= MARGEN_SATURACION);

  // Print CSV row.
  Serial.print(millis());           Serial.print(";");
  Serial.print(comboActual);        Serial.print(";");
  Serial.print(lLED);               Serial.print(";");
  Serial.print(lAVG);               Serial.print(";");
  Serial.print(lSR);                Serial.print(";");
  Serial.print(lPW);                Serial.print(";");
  Serial.print(lADC);               Serial.print(";");
  Serial.print(redMed);             Serial.print(";");
  Serial.print(irMed);              Serial.print(";");
  Serial.print(redNorm, 5);         Serial.print(";");
  Serial.print(irNorm,  5);         Serial.print(";");
  // SpO2: valid value, or 0.0 when it cannot be calculated.
  Serial.print(spO2OK ? spO2 : 0.0f, 1); Serial.print(";");
  Serial.print(spO2OK ? 1 : 0);     Serial.print(";");
  // BPM: valid value when at least 2 beats were detected, or 0.0.
  Serial.print(beats >= 2 ? bpmEst : 0.0f, 1); Serial.print(";");
  Serial.print(beats);              Serial.print(";");
  Serial.print(temp, 4);            Serial.print(";");
  Serial.print(msCiclo, 1);         Serial.print(";");
  Serial.print(extra);              Serial.print(";");
  Serial.print(estable  ? 1 : 0);   Serial.print(";");
  Serial.print(dedo     ? 1 : 0);   Serial.print(";");
  Serial.println(saturado ? 1 : 0);

  comboActual++;
}

/******************************************************************************
 * Full sweep with SR x PW validation
 *****************************************************************************/
void sweepMeasurements() {
  for (int iLED = 0; iLED < N_LED; iLED++) {
    for (int iAVG = 0; iAVG < N_AVG; iAVG++) {
      for (int iSR  = 0; iSR  < N_SR;  iSR++) {
        for (int iPW  = 0; iPW  < N_PW;  iPW++) {

          if (!combinacionValida(sampleRates[iSR], pulseWidths[iPW])) {
            combosOmitidos++;
            continue;
          }

          for (int iADC = 0; iADC < N_ADC; iADC++) {
            applyAndMeasure(
              ledCurrents[iLED], sampleAvgs[iAVG],
              sampleRates[iSR],  pulseWidths[iPW],
              adcRanges[iADC],
              etiquetasLED[iLED], etiquetasAVG[iAVG],
              etiquetasSR[iSR],   etiquetasPW[iPW],
              etiquetasADC[iADC]
            );
          }
        }
      }
    }
  }

  Serial.print(F("# SWEEP_COMPLETE - executed: "));
  Serial.print(comboActual);
  Serial.print(F(" - skipped: "));
  Serial.println(combosOmitidos);
}

/******************************************************************************
 * Startup SR x PW validity table
 *****************************************************************************/
void imprimirTablaValidez() {
  Serial.println(F("  SR x PW table - Y valid / N invalid:"));
  Serial.println(F("  SR       | 69us | 118us | 215us | 411us"));
  Serial.println(F("  ---------------------------------------------"));
  for (int iSR = 0; iSR < N_SR; iSR++) {
    Serial.print(F("  "));
    Serial.print(etiquetasSR[iSR]);
    int len = strlen(etiquetasSR[iSR]);
    for (int p = len; p < 8; p++) Serial.print(' ');
    Serial.print(F(" |  "));
    for (int iPW = 0; iPW < N_PW; iPW++) {
      Serial.print(combinacionValida(sampleRates[iSR], pulseWidths[iPW])
                   ? F("  Y  |") : F("  N  |"));
    }
    Serial.println();
  }
  Serial.println();
}

/******************************************************************************
 * Arduino setup and loop
 *****************************************************************************/
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Sensor not found. Check SDA=6 and SCL=7."));
    while (1);
  }

  // Calculate valid combinations.
  uint32_t pwSrValidos = 0;
  for (int iSR = 0; iSR < N_SR; iSR++)
    for (int iPW = 0; iPW < N_PW; iPW++)
      if (combinacionValida(sampleRates[iSR], pulseWidths[iPW]))
        pwSrValidos++;

  uint32_t totalValidos = (uint32_t)N_LED * N_AVG * N_ADC * pwSrValidos;

  Serial.println(F("======================================================"));
  Serial.println(F(" MAX30102 UE0133 full configuration sweep"));
  Serial.println(F(" Measures: Red, IR, SpO2, BPM, temperature"));
  Serial.println(F("======================================================"));
  Serial.print(F("  LED currents    : ")); Serial.println(N_LED);
  Serial.print(F("  Sample averages : ")); Serial.print(N_AVG); Serial.println(F(" (1,2,4,8,16,32)"));
  Serial.print(F("  Sample rates    : ")); Serial.print(N_SR);  Serial.println(F(" (50-3200 sps)"));
  Serial.print(F("  Pulse widths    : ")); Serial.println(N_PW);
  Serial.print(F("  ADC ranges      : ")); Serial.println(N_ADC);
  Serial.println(F("  ----------------------------------------------------"));
  Serial.print(F("  Valid combos    : ")); Serial.println(totalValidos);
  Serial.print(F("  BPM window      : ")); Serial.print(VENTANA_BPM_MS / 1000); Serial.println(F("s fixed"));
  Serial.println(F("  SpO2 = 110 - 25 x R, R=(AC_Red/DC_Red)/(AC_IR/DC_IR)"));
  Serial.println(F("  SpO2_valid=0 means no finger, saturation, or insufficient AC"));
  Serial.println();

  imprimirTablaValidez();
  imprimirCabecera();
}

void loop() {
  sweepMeasurements();
  Serial.println(F("# Sweep complete. Press any key to repeat."));
  while (!Serial.available());
  while (Serial.available()) Serial.read();
  comboActual    = 0;
  combosOmitidos = 0;
  imprimirCabecera();
}
