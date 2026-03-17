# SmartJunction AI — 4-Junction Edge AI Traffic System

> **Real-time urban traffic monitoring and control using ESP32, ESP32-CAM, YOLOv8, MQTT, and a live web dashboard.**

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   4-Junction Intersection                    │
│                                                             │
│         [J1]              [J2]                              │
│      ESP32+CAM         ESP32+CAM                            │
│      SENSOR+LED        SENSOR+LED                           │
│           │                 │                               │
│           └────────┬────────┘                               │
│                    │  WiFi MQTT                              │
│         [J3]       │       [J4]                             │
│      ESP32+CAM ────┴──── ESP32+CAM                          │
│      SENSOR+LED        SENSOR+LED                           │
└────────────────────╥───────────────────────────────────────┘
                     ║ MQTT (1883)
          ┌──────────╨──────────┐
          │   Edge AI Server    │  (PC / Laptop)
          │  ┌───────────────┐  │
          │  │ Node.js + ONNX │  │  ← pulls JPEG frames
          │  │ YOLOv8 Inference│  │    from each ESP32-CAM
          │  └──────┬────────┘  │
          │         │ MQTT pub  │
          │  ┌──────▼────────┐  │
          │  │ Express + WS  │  │
          │  │ Live Dashboard │  │
          │  └───────────────┘  │
          └─────────────────────┘
                     │
          http://localhost:5000
                     │
          ┌──────────▼──────────┐
          │   Live Dashboard    │
          │  • 4 junction cams  │
          │  • Signal control   │
          │  • Emergency banner │
          │  • Alerts feed      │
          │  • Intersection map │
          └─────────────────────┘
```

---

## ✨ Features

| Feature | Detail |
|---------|--------|
| 🚑 Emergency vehicle detection | Ambulance / fire truck → lane cleared, other 3 junctions force RED |
| 🚨 Adjacent junction alert | Emergency broadcast via MQTT to all 4 ESP32 nodes instantly |
| 🚗 Traffic density management | YOLOv8 vehicle count → dynamic green-time extension for high-traffic lanes |
| 💥 Accident / illegal activity | Multi-object heuristic → patrol alert dispatched |
| 📷 Live camera feeds | MJPEG stream from each ESP32-CAM refreshed on dashboard in real time |
| 🎛️ Manual controls | Per-junction RED / GREEN / YELLOW / AUTO / PATROL buttons on dashboard |
| 📊 KPI strip | Total vehicles, emergency count, avg traffic, cameras online |
| 🗺️ Intersection map | Visual mini-map of all 4 junctions with live signal state |
| 🔔 Alerts log | Timestamped log of all events (ambulance, accident, patrol) |

---

## 📦 Hardware per Junction (×4 total)

| Component | Role |
|-----------|------|
| **ESP32 Dev Module** | WiFi brain, signal control, UART to Mega |
| **ESP32-CAM (AI Thinker)** | Live MJPEG stream for YOLO inference |
| **Arduino Mega** | IR + Ultrasonic sensor aggregation → UART JSON |
| **IR sensor** | Crosswalk beam presence |
| **Ultrasonic (HC-SR04)** | Approaching vehicle distance |
| **PIR sensor** | Pedestrian motion |
| **3× LEDs (R/Y/G)** | Traffic signal |
| **Servo motor** | Barrier gate |
| **Active buzzer** | Emergency / pedestrian alert |
| **OLED 128×64 (I²C)** | Local status display |

---

## 📂 File Structure

```
SmartJunction_AI/
├── esp32_brain/
│   └── esp32_brain.ino         # ESP32 firmware (flash to each junction board)
├── esp32cam_stream/
│   └── esp32cam_stream.ino     # ESP32-CAM firmware (stream + MQTT heartbeat)
├── mega_sensors/
│   └── mega_sensors.ino        # Arduino Mega sensor aggregator
├── edge_ai_server/
│   ├── server.js               # Node.js Edge Server (YOLOv8 + Express + MQTT)
│   ├── package.json            # Node.js dependencies
│   ├── .env                    # Configuration (Camera IPs, MQTT)
│   ├── start.bat               # Windows quick-start script
│   └── public/
│       └── dashboard.html      # Live web dashboard
└── README.md
```

---

## ⚡ Quick Start

### 1. Set up MQTT Broker (Mosquitto)
```bash
# Windows — download from mosquitto.org
# then run:
mosquitto -v
```

### 2. Set up Edge AI Server
```bash
cd edge_ai_server
npm install
node download-model.js          # Downloads yolov8n.onnx
npm start
```

Dashboard opens at **http://localhost:5000**

### 3. Flash ESP32 (one per junction)
1. Open `esp32_brain/esp32_brain.ino` in Arduino IDE
2. Change `JUNCTION_ID` → `"J1"` (or J2/J3/J4)
3. Fill in `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`
4. Select **Board**: *ESP32 Dev Module*
5. Flash

### 4. Flash ESP32-CAM (one per junction)
1. Open `esp32cam_stream/esp32cam_stream.ino`
2. Change `JUNCTION_ID` to match
3. Fill in WiFi credentials
4. Select **Board**: *AI Thinker ESP32-CAM*
5. **Partition**: Huge APP (3MB, No OTA)
6. Flash

---

## 🔧 MQTT Topics Reference

| Topic | Direction | Description |
|-------|-----------|-------------|
| `smartjunction/J1/sensors` | ESP32 → Server | Sensor telemetry (dist, IR, PIR, signal, etc.) |
| `smartjunction/J1/camera` | ESP32-CAM → Server | Camera registration & heartbeat |
| `smartjunction/J1/vision` | Server → ESP32 | YOLO inference results |
| `smartjunction/J1/cmd` | Dashboard → ESP32 | Signal control commands |
| `smartjunction/emergency` | Broadcast | Emergency vehicle events |
| `smartjunction/alerts` | All nodes | Incidents log |

---

## 🚦 Signal Priority Logic

```
Priority (highest → lowest)
┌──────────────────────────────────────────────┐
│ 1. Emergency vehicle (ambulance/fire truck)  │  → Lane GREEN, others RED
│ 2. Manual override from dashboard            │  → Dashboard-set signal
│ 3. High traffic density (score > 70%)        │  → Extended green time
│ 4. Normal adaptive cycle (15–40 s green)     │  → Auto slot allocation
└──────────────────────────────────────────────┘
```

---

## 🌐 Dashboard API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Live dashboard |
| `/api/state` | GET | Full system state (JSON) |
| `/api/state/<J1>` | GET | Single junction state |
| `/api/alerts` | GET | All alerts log |
| `/api/cameras` | GET | Camera URLs & status |
| `/api/frame/<J1>` | GET | Latest JPEG frame (proxied) |
| `/api/cmd/<J1>/<CMD>` | POST | Send command to junction |
| `/api/emergency/clear` | POST | Clear emergency across all junctions |

---

## 🛠️ Configuration

Edit `edge_ai_server/.env`:

```env
J1_CAM_IP=192.168.1.101   # IP of Junction 1 ESP32-CAM
J2_CAM_IP=192.168.1.102
J3_CAM_IP=192.168.1.103
J4_CAM_IP=192.168.1.104
YOLO_MODEL=yolov8n.pt     # nano=fastest, s=balanced, m=accurate
INFER_FPS=2.0             # inference frames per second per camera
CONF_THRESH=0.45          # YOLO confidence threshold
```

---

## ⚠️ Notes

- Flash a **different `JUNCTION_ID`** (`"J1"`–`"J4"`) to each ESP32 and ESP32-CAM pair before deploying.
- All 4 junction nodes must use the same WiFi network as the edge server.
- YOLOv8 `yolov8n.pt` downloads automatically on first run (~6 MB).
- For best ambulance/fire truck detection, fine-tune on a custom dataset or use `yolov8s.pt` as a base.