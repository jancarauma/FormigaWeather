# FormigaWeather | Estação Formiga
### Estação Meteorológica com Inteligência Artificial e de Baixo Custo

A low-cost, open-source IoT weather station built on the ESP8266, with a responsive local web dashboard, persistent on-device data logging, and an optional AI assistant for interpreting the readings.

Blog Post: https://www.carauma.com/estacao-meteorologica-baixo-custo-esp8266-formiga

<img width="1897" height="1995" alt="image" src="https://github.com/user-attachments/assets/19f39bbf-b0db-40ae-90b4-e0345db35f0d" />

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)

## Overview

FormigaWeather turns a NodeMCU ESP8266 and a handful of common sensors into a self-contained environmental monitoring station. It measures air quality, temperature, humidity, atmospheric pressure, and rainfall, and serves the data through a web interface hosted directly on the microcontroller — no cloud service, no subscription, no external server required.

The project was built with a specific goal in mind: make it possible for schools, hobbyists, and makers to build and understand a real sensor network for a total cost of around US$30, using parts that are easy to find and a codebase that is easy to read and modify.

## Features

- **Self-hosted web dashboard.** The ESP8266 serves its own responsive site at `http://estacaoformiga.local`, with live charts for temperature, humidity, pressure, and air quality.
- **Works with or without an existing Wi-Fi network.** If no network is available, the device automatically starts its own access point so you can still connect and view the data.
- **Persistent on-device history.** Sensor readings are logged to a compact circular buffer stored in flash (LittleFS), so historical data survives power loss and reboots — no external database needed.
- **CSV export and system logs.** Download the recorded history as a CSV file, or review a running log of connection and sensor events, directly from the dashboard.
- **JSON API.** Simple HTTP endpoints (`/dados`, `/historico`, `/logs`) expose current readings, stored history, and diagnostics for anyone who wants to build their own client or integration.
- **Optional AI assistant.** With a free Gemini API key, the dashboard includes "Ana," a conversational assistant that can answer questions about current conditions in plain language. It is entirely optional and the station works normally without it.
- **Light and dark themes**, sensor status badges (ok / out of range / offline), and graceful handling of sensor failures.

## Hardware

| Component        | Function                          | Qty |
|-------------------|-----------------------------------|-----|
| NodeMCU ESP8266   | Wi-Fi microcontroller             | 1   |
| MQ-135            | Air quality (CO2 / VOC)           | 1   |
| BMP180            | Atmospheric pressure and altitude | 1   |
| DHT11             | Temperature and humidity          | 1   |
| MH-RD rain sensor | Rain detection                    | 1   |
| 10 kΩ resistor    | Pull-up for the DHT11             | 1   |
| Breadboard        | Prototyping board                 | 1   |
| Jumper wires      | Connections                       | —   |

Estimated total cost: **R$ 143.48** (approx. US$25–30, prices from Brazilian retailers, March 2025). Prices will vary by region and supplier.

<img width="960" height="1280" alt="image" src="https://github.com/user-attachments/assets/b7c21abd-a138-4ac0-8437-f2ac3494cff2" />

### Wiring

- DHT11 → D4 (with a 10 kΩ pull-up resistor between VCC and signal)
- BMP180 → I2C (SDA: D2, SCL: D1)
- MQ-135 → A0 (analog input)
- MH-RD → D5 (digital input)

<img width="1779" height="987" alt="image" src="https://github.com/user-attachments/assets/a2301d3f-722b-4818-93e4-79b18ab914e3" />

## Getting Started

### Requirements

- Arduino IDE 1.8.18 or later
- ESP8266 board package
- Libraries: `Adafruit BMP085 Library`, `DHT sensor library`

### Setup

1. In Arduino IDE, go to **File → Preferences** and add the following to "Additional Boards Manager URLs":
   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
2. Go to **Tools → Board → Boards Manager**, search for "ESP8266," and install the package.
3. Under **Tools → Board**, select **NodeMCU 1.0 (ESP-12E Module)**.
4. Install the two required libraries via **Sketch → Include Library → Add .ZIP Library**:
   - [Adafruit_Sensor-master.zip](https://github.com/jancarauma/FormigaWeather/blob/main/Adafruit_Sensor-master.zip)
   - [DHT-sensor-library.zip](https://github.com/jancarauma/FormigaWeather/blob/main/DHT-sensor-library.zip)

### Installation

```bash
git clone https://github.com/jancarauma/FormigaWeather.git
```

1. Open `estacao_formiga.ino` in the Arduino IDE.
2. Set your Wi-Fi credentials in the `ssid` and `password` variables. If you leave them as-is (or have no Wi-Fi available), the ESP8266 will create its own network, `Estacao_Formiga`, with the password `senha123`.
3. Double-check the wiring against the table above.
4. Connect the ESP8266 via USB, select the correct port, and upload the sketch.
5. Open the Serial Monitor to confirm the device started correctly.
6. Connect your phone or computer to the same network as the station and visit `http://estacaoformiga.local`, or use the IP address printed in the Serial Monitor.

### Enabling the AI assistant (optional)

To enable "Ana," the built-in conversational assistant:

1. Create a free API key at [Google AI Studio](https://aistudio.google.com/).
2. Paste it into the sketch:
   ```cpp
   const char* GEMINI_KEY = "your-key-here";
   ```
3. Recompile and upload. The assistant will appear automatically on the dashboard, with access to the station's current sensor readings. Leave the key blank to keep the dashboard running without it.

## Using the Dashboard

Once connected to the station's network:

- Visit `http://estacaoformiga.local` (or the IP shown in the Serial Monitor) to view live readings and charts.
- **Export CSV** downloads the session's recorded data in the format: `Date, Time, Temperature, Humidity, Pressure, AirQuality, Rain`.
- **System Logs** shows a running history of connection events and sensor errors.
- **Stored history** shows how much data is currently saved on the device and lets you clear it if needed.

## Project Structure

The firmware is written as a single Arduino sketch. HTML/CSS/JS for the dashboard is stored in flash (`PROGMEM`) and streamed to the browser in chunks, keeping RAM usage low and stable even on the ESP8266's limited memory. Sensor readings are validated against plausibility ranges before being reported, and each sensor's status (ok, out of range, or offline) is tracked and surfaced in the interface rather than silently ignored.

## Contributing

Contributions are welcome. To propose a change:

1. Fork the repository
2. Create a branch (`git checkout -b feature/my-feature`)
3. Commit your changes (`git commit -m 'Add my feature'`)
4. Push the branch (`git push origin feature/my-feature`)
5. Open a pull request

## License

Distributed under the MIT License. See `LICENSE` for details.

## Acknowledgments

This project grew out of collaboration with Dr. Paulo Marotti, professor at the Federal University of Roraima (UFRR), and his work with students in the municipality of Uiramutã, Roraima. Thanks also to the Arduino/ESP8266 community and the maintainers of the libraries this project relies on.
