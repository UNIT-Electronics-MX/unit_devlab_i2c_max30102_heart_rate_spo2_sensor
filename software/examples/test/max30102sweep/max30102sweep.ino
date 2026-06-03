/** @file max30102_sweep_completo.ino
 *
 * @brief Sweep COMPLETO de todas las configuraciones válidas del MAX30102
 *        Módulo UE0133 — Unit Electronics
 *
 *        Mide: Red raw, IR raw, BPM y SpO2
 *
 *        Cubre TODOS los valores del datasheet:
 *          LED current  : 5 valores  (0x0F a 0xFF)
 *          Sample avg   : 6 valores  (1,2,4,8,16,32)   Tabla 3  pág.17
 *          Sample rate  : 8 valores  (50 a 3200 sps)    Tabla 6  pág.19
 *          Pulse width  : 4 valores  (69 a 411 µs)      Tabla 7  pág.19
 *          ADC range    : 4 valores  (2048 a 16384 nA)  Tabla 5  pág.18
 *
 *        Combinaciones SR×PW validadas contra Tabla 11 (pág.23):
 *          PW= 69µs: SR 50–3200 → 8 válidos
 *          PW=118µs: SR 50–1600 → 7 válidos
 *          PW=215µs: SR 50–1000 → 6 válidos
 *          PW=411µs: SR 50– 800 → 5 válidos
 *          Total PW×SR válidos: 26 de 32 posibles
 *
 *        Total combinaciones válidas:
 *          5 LED × 6 AVG × 4 ADC × 26 PW×SR = 3120
 *
 *        Cálculo de SpO2 (datasheet pág. 9, 24):
 *          R = (AC_Red/DC_Red) / (AC_IR/DC_IR)
 *          SpO2 ≈ 110 - 25 × R
 *          AC = desviación estándar de la ventana
 *          DC = promedio de la ventana
 *
 * @author Jonathan Mejorado Lopez
 * @bug    No known bugs.
 */

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

#define SDA_PIN  6
#define SCL_PIN  7

MAX30105 sensor;

/******************************************************************************
 * Parámetros de medición
 *****************************************************************************/
#define CICLOS_WARMUP_HW      5       // Ciclos ALC settling (hardware)
#define MUESTRAS_WARMUP_BEAT  15      // Muestras para inicializar checkForBeat()
#define CICLOS_MEDICION_ADC   40      // Ciclos para promedio Red/IR y SpO2
                                      // 40 ciclos = ventana mínima para AC/DC
#define UMBRAL_ESTAB_PCT      5.0f    // % varianza para señal estable
#define MAX_CICLOS_EXTRA      20      // Ciclos extra max esperando estabilidad
#define VENTANA_BPM_MS        10000UL // Ventana BPM fija independiente de config
#define MARGEN_SATURACION     262130L // IR >= este valor = ADC saturado

/******************************************************************************
 * Sensor config values
 *****************************************************************************/

// LED current — I(mA) = registro × 0.2  (datasheet pág. 20)
byte ledCurrents[] = {
  0x0F,   //  ~3.0 mA
  0x1F,   //  ~6.4 mA
  0x3F,   // ~12.5 mA
  0x7F,   // ~25.4 mA
  0xFF    // ~50.0 mA
};
const char* etiquetasLED[] = {
  "3.0mA","6.2mA","12.6mA","25.4mA","51.0mA"
};
#define N_LED 5

// Sample Average — Tabla 3 pág.17 — COMPLETO incluyendo 16 y 32
byte sampleAvgs[] = { 1, 2, 4, 8, 16, 32 };
const char* etiquetasAVG[] = {
  "AVG1","AVG2","AVG4","AVG8","AVG16","AVG32"
};
#define N_AVG 6

// Sample Rate — Tabla 6 pág.19 — COMPLETO incluyendo 800 a 3200
int sampleRates[] = { 50, 100, 200, 400, 800, 1000, 1600, 3200 };
const char* etiquetasSR[] = {
  "50sps","100sps","200sps","400sps","800sps","1000sps","1600sps","3200sps"
};
#define N_SR 8

// Pulse Width — Tabla 7 pág.19
int pulseWidths[] = { 69, 118, 215, 411 };
const char* etiquetasPW[] = {
  "69us_15b","118us_16b","215us_17b","411us_18b"
};
#define N_PW 4

// ADC Range — Tabla 5 pág.18
int adcRanges[] = { 2048, 4096, 8192, 16384 };
const char* etiquetasADC[] = {
  "2048nA","4096nA","8192nA","16384nA"
};
#define N_ADC 4

#define LED_MODE       2    // SpO2: Red + IR — fijo
uint32_t comboActual    = 0;
uint32_t combosOmitidos = 0;

/******************************************************************************
 * Validación SR × PW según Tabla 11 datasheet pág.23
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

// Umbral de dedo escalado con el rango ADC activo
// Base 50000 counts con 2048nA (≈20% del FS)
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
 * Warmup de checkForBeat()
 * Alimenta sin contar para formar el promedio móvil interno
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
 * Verificar estabilidad por varianza
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
 * Ventana de medición combinada: ADC promedio + SpO2 + BPM
 *
 * Se captura UN solo buffer de muestras que sirve para los tres cálculos:
 *
 *   1. Red/IR promedio (DC) → valor crudo para el CSV
 *
 *   2. SpO2 (datasheet pág. 9, 24):
 *      DC_red = promedio de muestras Red
 *      DC_ir  = promedio de muestras IR
 *      AC_red = desviación estándar de muestras Red
 *      AC_ir  = desviación estándar de muestras IR
 *      R      = (AC_red / DC_red) / (AC_ir / DC_ir)
 *      SpO2   = 110.0 - 25.0 × R
 *      Rango válido: 70% a 100%
 *
 *   3. BPM mediante checkForBeat() en ventana de tiempo fija
 *      (se extiende la captura hasta completar VENTANA_BPM_MS)
 *
 * Requisito: dedo presente (irMed > umbral) y señal no saturada
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

  // ── Fase 1: captura de nADC muestras para promedio y SpO2 ─────────────
  while (cADC < nADC) {
    sensor.check();
    while (sensor.available() && cADC < nADC) {
      long r = (long)sensor.getFIFORed();
      long i = (long)sensor.getFIFOIR();

      bufRed[cADC] = (float)r;
      bufIR [cADC] = (float)i;
      sumRed += r;
      sumIR  += i;

      // BPM simultáneo durante la fase ADC
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

  // ── Cálculo de SpO2 ────────────────────────────────────────────────────
  // AC = desviación estándar de la ventana (componente pulsátil)
  // Datasheet pág. 9: la señal PPG tiene componente DC (tejido estático)
  // y componente AC (variación por latido cardíaco)
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

    // Evitar división por cero y señales sin componente AC
    // Si AC < 0.1% del DC → sin onda PPG → SpO2 no calculable
    if (dcRed > 0 && dcIR > 0 &&
        acRed > dcRed * 0.001f &&
        acIR  > dcIR  * 0.001f) {

      float R = (acRed / dcRed) / (acIR / dcIR);

      // Curva de calibración empírica estándar (datasheet pág. 24)
      // SpO2 = 110 - 25 × R
      float spo2 = 110.0f - 25.0f * R;

      // Rango fisiológico válido: 70–100%
      // Fuera de rango = señal insuficiente o artefacto
      if (spo2 >= 70.0f && spo2 <= 100.0f) {
        spO2Est    = spo2;
        spO2Valido = true;
      }
    }
  }

  // ── Fase 2: extender captura hasta completar VENTANA_BPM_MS ──────────
  // Solo si quedan ms de ventana BPM por completar
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
 * Cabecera CSV
 *****************************************************************************/
void imprimirCabecera() {
  Serial.println(F("ms;#;LED_mA;AVG;SR;PW;ADC;"
                   "Red;IR;Red_norm;IR_norm;"
                   "SpO2;SpO2_valido;"
                   "BPM;Beats;"
                   "Temp_C;ms_ciclo;CiclosExtra;"
                   "Estable;Dedo;Saturado"));
}

/******************************************************************************
 * Aplicar configuración y medir todo
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

  // Warmup checkForBeat() — forma el promedio móvil antes de medir
  warmupBeat(umbral);

  // Medición unificada: ADC + SpO2 + BPM en una sola ventana
  long  redMed = 0, irMed = 0;
  float spO2   = 0.0f;
  bool  spO2OK = false;
  float bpmEst = 0.0f;
  int   beats  = 0;

  long irCheck = 0;
  {
    // Leer un valor rápido para saber si hay dedo antes de medirTodo()
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
    // Sin dedo: solo tomar promedio rápido sin BPM ni SpO2
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

  // Recalcular dedo/saturado con el promedio real
  dedo     = (irMed > umbral);
  saturado = (irMed >= MARGEN_SATURACION);

  // Imprimir línea CSV
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
  // SpO2: valor si válido, 0.0 si no calculable
  Serial.print(spO2OK ? spO2 : 0.0f, 1); Serial.print(";");
  Serial.print(spO2OK ? 1 : 0);     Serial.print(";");
  // BPM: valor si >= 2 beats detectados, 0.0 si no
  Serial.print(beats >= 2 ? bpmEst : 0.0f, 1);                                                                                  Serial.print(";");
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
 * Sweep completo con validación SR×PW
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

  Serial.print(F("# SWEEP_COMPLETO — ejecutados: "));
  Serial.print(comboActual);
  Serial.print(F(" — omitidos: "));
  Serial.println(combosOmitidos);
}

/******************************************************************************
 * Tabla de validez SR×PW al inicio
 *****************************************************************************/
void imprimirTablaValidez() {
  Serial.println(F("  Tabla SR × PW — ✓ válido / ✗ inválido (Tabla 11, pág.23):"));
  Serial.println(F("  SR       | 69us | 118us | 215us | 411us"));
  Serial.println(F("  ─────────────────────────────────────────────"));
  for (int iSR = 0; iSR < N_SR; iSR++) {
    Serial.print(F("  "));
    Serial.print(etiquetasSR[iSR]);
    int len = strlen(etiquetasSR[iSR]);
    for (int p = len; p < 8; p++) Serial.print(' ');
    Serial.print(F(" |  "));
    for (int iPW = 0; iPW < N_PW; iPW++) {
      Serial.print(combinacionValida(sampleRates[iSR], pulseWidths[iPW])
                   ? F("  ✓  |") : F("  ✗  |"));
    }
    Serial.println();
  }
  Serial.println();
}

/******************************************************************************
 * Setup y Loop
 *****************************************************************************/
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Sensor no encontrado. Verificar SDA=6 SCL=7."));
    while (1);
  }

  // Calcular combinaciones válidas
  uint32_t pwSrValidos = 0;
  for (int iSR = 0; iSR < N_SR; iSR++)
    for (int iPW = 0; iPW < N_PW; iPW++)
      if (combinacionValida(sampleRates[iSR], pulseWidths[iPW]))
        pwSrValidos++;

  uint32_t totalValidos = (uint32_t)N_LED * N_AVG * N_ADC * pwSrValidos;

  Serial.println(F("╔══════════════════════════════════════════════════════╗"));
  Serial.println(F("║  SWEEP COMPLETO MAX30102 UE0133 — Unit Electronics  ║"));
  Serial.println(F("║  Mide: Red, IR, SpO2, BPM, Temperatura             ║"));
  Serial.println(F("╚══════════════════════════════════════════════════════╝"));
  Serial.print  (F("  LED currents    : ")); Serial.println(N_LED);
  Serial.print  (F("  Sample averages : ")); Serial.print(N_AVG); Serial.println(F(" (1,2,4,8,16,32)"));
  Serial.print  (F("  Sample rates    : ")); Serial.print(N_SR);  Serial.println(F(" (50–3200 sps)"));
  Serial.print  (F("  Pulse widths    : ")); Serial.println(N_PW);
  Serial.print  (F("  ADC ranges      : ")); Serial.println(N_ADC);
  Serial.println(F("  ─────────────────────────────────────────────────────"));
  Serial.print  (F("  Combos válidos  : ")); Serial.println(totalValidos);
  Serial.print  (F("  Ventana BPM     : ")); Serial.print(VENTANA_BPM_MS/1000); Serial.println(F("s fijos"));
  Serial.println(F("  SpO2 = 110 - 25 × R   donde R=(AC_Red/DC_Red)/(AC_IR/DC_IR)"));
  Serial.println(F("  SpO2_valido=0 → sin dedo, saturado o AC insuficiente"));
  Serial.println();

  imprimirTablaValidez();
  imprimirCabecera();
}

void loop() {
  sweepMeasurements();
  Serial.println(F("# Sweep completo. Tecla para repetir."));
  while (!Serial.available());
  while  (Serial.available()) Serial.read();
  comboActual    = 0;
  combosOmitidos = 0;
  imprimirCabecera();
}
