# SmartJunction AI — 4-Junction Edge AI Traffic System

> **Real-time urban traffic monitoring and control using ESP32, ESP32-CAM, YOLOv8, MQTT, and a live web dashboard.**

---

## 🏗️ System Architecture

```text
┌─────────────────────────────────────────────────────────────┐
│                   4-Junction Intersection                   │
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
| 🚑 **Emergency Priority** | YOLO-based ambulance/fire truck detection → priority green at junction |
| 🚨 **Traffic Management** | AI-driven vehicle counting → dynamic signal timing adjustments |
| 📷 **Live Streaming** | Multi-camera MJPEG streams (up to 4 junctions) on one dashboard |
| 💥 **Accident Alert** | Heuristic alerts for collisions or suspicious traffic patterns |
| 🎛️ **Remote Control** | Manual override toggles for red/yellow/green/auto modes |
| 📊 **Real-time Dashboard** | Modern Node.js + Socket.IO interface with KPI stats and live logs |

---

## 📂 Project Layout

| Component | Path | Technology | Purpose |
|---|---|---|---|
| **Core Server** | `/edge_ai_server/` | Node.js + ONNX | YOLO Inference, MQTT, Post-processing, Dashboard Server |
| **Brain Nodes** | `/esp32_brain/` | ESP32 C++ | LED signals, Servo gates, MQTT logic |
| **Vision Nodes** | `/esp32cam_stream/` | ESP32-CAM | High-speed MJPEG stream & MQTT heartbeats |
| **Sensor Hub** | `/mega_sensors/` | Arduino Mega | Aggregates IR, Ultrasonic, PIR data via UART JSON |
| **Static UI**| `/frontend/` | HTML/JS | Premium standalone dashboard UI |

---

## 📦 Hardware per Junction (×4)

| Component | Role |
|-----------|------|
| **ESP32 Dev Module** | WiFi brain, signal control, UART to Mega |
| **ESP32-CAM (AI Thinker)** | Vision sensor for AI inference |
| **Arduino Mega** | HC-SR04 + PIR + IR aggregator |
| **3× LEDs (R/Y/G)** | Traffic signal status |
| **Servo motor** | Barrier gate mechanism |
| **OLED 0.96"** | Local junction debug status |

---

## ⚡ Quick Start

### 1. Set up MQTT (Mosquitto)
- Windows download from **mosquitto.org**
- Run: `mosquitto -v`

### 2. Launch Edge Server
```bash
cd edge_ai_server
npm install
node download-model.js    # Downloads yolov8n.onnx (~6 MB)
npm start
```

### 3. Flash Hardware
1. **Mega Hub**: Load `mega_sensors/mega_sensors.ino` to Arduino Mega.
2. **Brain Node**: Load `esp32_brain/esp32_brain.ino` to each ESP32. Set `JUNCTION_ID` (J1–J4).
3. **Vision Node**: Load `esp32cam_stream/esp32cam_stream.ino` to each ESP32-CAM.

### 4. Open Dashboard
Browser: **[http://localhost:5000](http://localhost:5000)**

---

## 🚦 Priority Logic (High → Low)

1.  **Emergency Override**: Ambulance/Fire detected → GREEN priority, others RED.
2.  **Manual Command**: Manual mode from Dashboard. 
3.  **Adaptive Traffic**: Higher vehicle volume gets longer green slots (15–40s).
4.  **Default Cycle**: Round-robin base timing.

---

## 🔧 MQTT Topics

- `smartjunction/+/sensors`: Telemetry (JSON)
- `smartjunction/+/vision`: Inference data from server
- `smartjunction/+/cmd`: Remote commands (RED, GREEN, etc.)
- `smartjunction/emergency`: Global emergency broadcasts
- `smartjunction/alerts`: Alert and incident log feed

---

## ⚠️ Notes
- Edit `edge_ai_server/.env` with your actual ESP32-CAM and MQTT IPs.
- Use **Huge APP** partition scheme for ESP32-CAM (Vision logic).
- Standard serial rate: **115200 baud** across all nodes.
