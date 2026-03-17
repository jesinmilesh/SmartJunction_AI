# SmartJunction AI 🚦

![Smart Junction Mockup](smart_junction_mockup_1773751949009.png)

An Arduino-based intelligent traffic junction controller that combines physical sensors, computer vision (Roboflow), and a live Node-RED dashboard to manage traffic flow in real-time.

---

## System Architecture

```
[HC-SR04] [IR Sensor] [PIR Sensor]
        ↓ (Serial UART)
  [Arduino Mega 2560]  ←── Sensor Hub (JSON @ 115200 baud)
        ↓ (TX1→RX2 via voltage divider)
  [ESP32 Dev Module]   ←── Brain: WiFi + MQTT + Logic + Servo + OLED
        ↑ (HTTP REST)          ↑ (MQTT pub/sub)
  [ESP32-CAM]          [Node-RED Dashboard on Laptop]
  /stream & /capture         gauges, chart, camera embed,
        ↑ (HTTPS/POST)        override buttons
  [Roboflow Inference API]
```

---

## Project Files

| File | Board | Purpose |
|---|---|---|
| `mega_sensors/mega_sensors.ino` | Arduino Mega 2560 | Reads HC-SR04 + IR + PIR, streams JSON |
| `esp32_brain/esp32_brain.ino` | ESP32 Dev Module | Brain: receives sensor data, runs decision logic, WiFi/MQTT, Roboflow, OLED |
| `esp32cam_stream/esp32cam_stream.ino` | AI Thinker ESP32-CAM | MJPEG stream server + `/capture` endpoint |
| `nodered_flow/smartjunction_flow.json` | Laptop (Node-RED) | Live dashboard: gauges, chart, camera, override buttons |

---

## Hardware Requirements

| Component | Qty | Notes |
|---|---|---|
| Arduino Mega 2560 | 1 | Sensor hub |
| ESP32 Dev Module | 1 | Brain board |
| AI Thinker ESP32-CAM | 1 | Vision stream |
| HC-SR04 Ultrasonic | 1 | Vehicle distance |
| IR Proximity Sensor | 1 | Crosswalk beam |
| PIR Motion Sensor | 1 | Pedestrian detection |
| SG90 Servo | 1 | Barrier gate |
| 0.96" OLED SSD1306 | 1 | Local status display |
| Red + Green LEDs | 2 each | Traffic signals |
| Active Buzzer | 1 | Pedestrian / emergency alert |
| 1 kΩ + 2 kΩ resistors | 1 set | Voltage divider (Mega 5V → ESP32 3.3V) |

---

## Wiring Summary

### Arduino Mega → Sensors

| Pin | Sensor | Notes |
|---|---|---|
| D9 (TRIG) | HC-SR04 Trigger | — |
| D10 (ECHO) | HC-SR04 Echo | — |
| D7 | IR Sensor OUT | Active-LOW (code inverts) |
| D6 | PIR Sensor OUT | Active-HIGH |

### Mega → ESP32 UART Link

```
Mega TX1 (pin 18) ──┬── 1 kΩ ── ESP32 RX2 (GPIO 16)
                    └── 2 kΩ ── GND
Mega GND ────────────────────── ESP32 GND
```
The resistor divider steps 5 V down to ~3.3 V to protect the ESP32.

### ESP32 → Outputs

| GPIO | Device |
|---|---|
| 25 | Red LED (220 Ω to GND) |
| 26 | Green LED (220 Ω to GND) |
| 27 | Buzzer |
| 14 | Servo signal |
| 21 (SDA) | OLED SDA |
| 22 (SCL) | OLED SCL |

---

## Software Setup

### Step 1 — Arduino IDE Board Support

Open **File → Preferences** and add this URL to *Additional Boards Manager URLs*:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

Then install **ESP32 by Espressif Systems** via **Tools → Board → Boards Manager**.

### Step 2 — Required Libraries

Install via **Sketch → Include Library → Manage Libraries**:

| Library | Author |
|---|---|
| ArduinoJson | Benoit Blanchon |
| PubSubClient | Nick O'Leary |
| ESP32Servo | Kevin Harrington |
| NewPing | Tim Eckel |
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |

`HTTPClient`, `WiFi`, and `base64` are built into the ESP32 Arduino core.

### Step 3 — Configure Credentials

In **both** `esp32_brain.ino` and `esp32cam_stream.ino`, fill in:

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER   = "192.168.1.100";  // your laptop's LAN IP
```

In `esp32_brain.ino` also fill in:

```cpp
const char* RF_API_KEY    = "YOUR_ROBOFLOW_API_KEY";
const char* RF_MODEL_ID   = "your-model-name/1";
const char* CAM_STREAM_IP = "192.168.1.105";  // ESP32-CAM IP (shown in Serial Monitor)
```

### Step 4 — Flash Order

1. Flash `mega_sensors.ino` → Arduino Mega (port: COMx, board: Arduino Mega 2560)
2. Flash `esp32cam_stream.ino` → ESP32-CAM (**Board: AI Thinker ESP32-CAM**, Partition: Huge APP, PSRAM: Enabled)
3. Open Serial Monitor @ 115200 to get CAM IP → paste into `CAM_STREAM_IP` in esp32_brain.ino
4. Flash `esp32_brain.ino` → ESP32 Dev Module

---

## Roboflow Setup

1. Create a free account at [roboflow.com](https://roboflow.com)
2. New Project → **Object Detection**
3. Upload 50-100 images with vehicles and pedestrians (or use Roboflow Universe datasets)
4. Annotate: label `vehicle` and `person`
5. Train → **Roboflow 3.0 Fast** (free, ~10 min)
6. Deploy → copy your **API Key** and **Model ID**

---

## Node-RED Dashboard

### Install Node-RED

```bash
npm install -g node-red
node-red
```

Open [http://localhost:1880](http://localhost:1880).

### Install Mosquitto MQTT Broker (on laptop)

- **Windows**: [mosquitto.org/download](https://mosquitto.org/download/) — just install and run as a service
- **Linux/Mac**: `sudo apt install mosquitto` or `brew install mosquitto`

### Install Dashboard Palette

In Node-RED: **☰ → Manage Palette → Install** → search `node-red-dashboard`

### Import the Flow

**☰ → Import** → paste the contents of `nodered_flow/smartjunction_flow.json` → Deploy.

Dashboard lives at: [http://localhost:1880/ui](http://localhost:1880/ui)

---

## MQTT Topics

| Topic | Direction | Payload |
|---|---|---|
| `smartjunction/sensors` | ESP32 → Node-RED | `{"dist":42,"ir":0,"pir":0,"cong":60}` |
| `smartjunction/vision` | ESP32 → Node-RED | `{"vehicles":2,"persons":0}` |
| `smartjunction/status` | ESP32 → Node-RED | `"NORMAL"` / `"PEDESTRIAN_HOLD"` / `"EMERGENCY"` |
| `smartjunction/override` | Node-RED → ESP32 | `"GREEN"` / `"RED"` / `"EMERGENCY"` / `"CLEAR"` |

---

## Decision Logic (ESP32 Brain)

```
Priority 1 (highest): EMERGENCY mode       → All-red + barrier down + buzzer
Priority 2:           Pedestrian detected  → Red + barrier + buzzer 3s
Priority 3:           Vehicle < 80cm       → Predictive GREEN + raise barrier
Priority 4 (default): Clear               → GREEN + raise barrier + silent
```

Remote overrides from Node-RED buttons override priorities 2-4.

---

## License

MIT — free to use for educational and research purposes.