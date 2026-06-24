# MAX30102 Software Examples

Arduino sketches for the UNIT Electronics UE0133 MAX30102 Heart Rate and
SpO2 Sensor, validated with the Pulsar C6 (ESP32-C6).

> These examples are intended for development and validation. They are not
> medical diagnostic tools.

## Hardware

| MAX30102 pin | Pulsar C6 |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| QWIIC SDA | GPIO6 |
| QWIIC SCL | GPIO7 |
| INT | GPIO4 / D7 |

## Examples

| Example | Purpose |
| --- | --- |
| [test_BPM-SpO2](test/test_BPM-SpO2/test_BPM-SpO2.ino) | Reads IR/RED data and estimates BPM and SpO2 with a simple serial monitor output. |
| [max30102sweep](test/max30102sweep/max30102sweep.ino) | Sweeps valid MAX30102 LED current, sample average, sample rate, pulse width, and ADC range combinations. |
| [interrupttest](test/interrupttest/interrupttest/interrupttest.ino) | Validates the INT pin using the data-ready interrupt. |
| [interruptvalid](test/interrupttest/interruptvalid/interruptvalid.ino) | Validates proximity interrupt behavior. |

## Requirements

- Arduino IDE or `arduino-cli`
- ESP32 board support with Pulsar C6 / ESP32-C6 target
- `Wire`
- SparkFun MAX3010x-compatible library that provides:
  - `MAX30105.h`
  - `heartRate.h`
  - `spo2_algorithm.h`

## Usage

1. Connect the MAX30102 module using the QWIIC connector.
2. Open one of the `.ino` files from the table above.
3. Select the Pulsar C6 / ESP32-C6 board and the correct serial port.
4. Upload the sketch.
5. Open the serial monitor at `115200` baud.

For BPM and SpO2 tests, place your finger gently on the sensor and wait for the
signal to stabilize before reading the output.
