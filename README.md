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
| **Firmware Hub** | `/firmware/` | C++ (Arduino/ESP32) | Multi-node source code for Brain, Vision, and Sensors |
| **Standalone UI**| `/frontend/` | HTML/JS/CSS | Premium 4.1 HUD dashboard UI (Single Viewport) |
| **Node-RED** | `/flows/` | JSON / JS | Industrial logic, MQTT bridging, and legacy dashboard |

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

### Install Dashboard Palette

In Node-RED: **☰ → Manage Palette → Install** → search `node-red-dashboard`

### Import the Flow

**☰ → Import** → paste the contents of `flows/smartjunction_flow.json` → Deploy.

---

## 🌐 Web Dashboard (Modern Interface)

The project includes a premium, high-performance web dashboard located in the `frontend/` folder.

### 🚀 Live Demo
**[Live Deployment Site](https://jesinmilesh.github.io/SmartJunction_AI/)**
*(Automatically deployed via GitHub Actions)*

### How to Launch Locally
From the root of the project, run:
```bash
npm run frontend
```
Then open [http://localhost:3000](http://localhost:3000) in your browser.

**Features:**
- **Solar-Neon UI**: Sleek dark mode with glassmorphism.
- **Reactive Gauges**: Real-time traffic density and proximity visualization.
- **AI Detections**: Live log of vehicles and pedestrians.
- **Priority Overrides**: Send manual commands directly from the UI.

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
