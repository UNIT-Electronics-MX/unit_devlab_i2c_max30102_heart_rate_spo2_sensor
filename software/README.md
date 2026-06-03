# MAX30102 Hearth Rate SpO2 Sensor

**Arduino Examples – Pulsar C6 (ESP32-C6)**

## Overview

This repository provides reference test code for the **MAX30102 Hearth Rate SpO2 Sensor**, validated on the PUlSAR C6
(ESP32-C6)platform.

The goal of this project is to offer a simple, reliable, and well-documented baseline for hearth rate and spo2 measurements,
using the ESP32 I2C QWIIC Connector.

## Hardware Connection(PULSAR C6)

<div align="center">

| MAX30102 Pin | Pulsar C6    |
| ---------- | ------------ |
| VCC        | 3.3 V        |
| GND        | GND          |
| QWIIC SDA  | GPIO6        |
| QWIIC SCL  | GPIO7        |
| INT Pin    | D7           |

</div>


## Installation

1. Clone the repository:
    ```sh
    git clone https://github.com/UNIT-Electronics-MX/unit_devlab_i2c_max30102_heart_rate_spo2_sensor.git
    ```
2. Follow the setup instructions in the `docs/` directory.


## Usage

- Connect the board to your computer, with the QWIIC Connector, like in the image below.
<div align="center">

  <img src="../hardware/resources/img/connection.jpeg" width="450px" alt="Development Board">
  <p><em></em></p>

</div>

- Open the Arduino IDE and open the code of your preference,this codes can found in the [test folder](examples/test)

- You can found 3 code examples:
    1. [test_BPM-SpO2](examples/test/test_BPM-SpO2/test_BPM-SpO2.ino): Example for test with default settings of the sensor, the measure of SpO2, hearth rate. For better measurements, do not press your finger too hard on the sensor, and wait until the signal stabilizes. 

    2. [max30102sweep](examples/test/max30102sweep/max30102sweep.ino): This example code tests all available sensor settings. Each sample is taken with a 10-second delay between measurements to obtain reliable heart rate readings. The full test may take 4 hours or more to collect all signals.

    3. [interrupttest](examples/test/interrupttest/): This example tests the interrupt pin connected to the Pulsar C6. The interrupt is triggered when a change is detected. In this case, the Data Ready flag and the proximity detection feature are used to activate the interrupt.

    On the Pulsar C6, the interrupt handler must be defined with the function`IRAM_ATTR` to ensure proper interrupt handling on the available pins.


## Support

For questions or issues, please open an issue on GitHub.
