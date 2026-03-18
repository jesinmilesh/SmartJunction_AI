/**
 * SmartJunction AI — Edge Server (Node.js v2.1)
 * ==========================================
 * Stack:
 *   • Express          — REST API + static serving
 *   • Socket.IO        — real-time push to dashboard
 *   • mqtt (npm)       — MQTT communication with all 4 ESP32 nodes
 *   • axios            — pulls JPEG frames from ESP32-CAM nodes
 *   • onnxruntime-node — YOLOv8n ONNX model inference (CPU)
 *   • sharp            — JPEG decode + resize + crop (RGB only)
 *   • dotenv           — .env config file
 *
 * Run:
 *   node server.js         (or: npm start)
 *
 * Dashboard:
 *   http://localhost:5000
 */

'use strict';

require('dotenv').config();
const express    = require('express');
const http       = require('http');
const path       = require('path');
const { Server } = require('socket.io');
const mqttLib    = require('mqtt');
const axios      = require('axios');
const sharp      = require('sharp');
const ort        = require('onnxruntime-node');
const fs         = require('fs');
const os         = require('os'); // Import 'os' module
const { Aedes }  = require('aedes');
const aedes      = new Aedes();
const net        = require('net');



// ─────────────────────────────────────────────────────────────
//  Configuration  (also read from .env)
// ─────────────────────────────────────────────────────────────
const MQTT_HOST   = process.env.MQTT_HOST   || 'localhost';
const MQTT_PORT   = parseInt(process.env.MQTT_PORT   || '1883', 10);
const WEB_PORT    = parseInt(process.env.WEB_PORT    || '5000', 10);
const YOLO_MODEL  = process.env.YOLO_MODEL  || path.join(__dirname, 'yolov8n.onnx');
const CONF_THRESH = parseFloat(process.env.CONF_THRESH || '0.45');
const INFER_MS    = Math.round(1000 / parseFloat(process.env.INFER_FPS || '8')); // Bumped to 8 FPS for fluid motion

// Roboflow Cloud Intelligence (Dataset v3)
const RF_API_KEY  = process.env.ROBOFLOW_API_KEY;
const RF_MODEL_ID = process.env.ROBOFLOW_MODEL_ID || 'smartjunction-traffic/3';
const RF_URL      = `https://detect.roboflow.com/${RF_MODEL_ID}?api_key=${RF_API_KEY}`;
const RF_THROTTLE = 1000; // FAST REACTION: Only wait 1 second between high-precision cloud checks

// How often (ms) to re-broadcast emergency per junction (throttle)
const EMERGENCY_BROADCAST_INTERVAL = 10000;

// Junction ID → camera IP mapping
const JUNCTIONS = {
  J1: process.env.J1_CAM_IP || '10.152.59.144:8080',
  J2: process.env.J2_CAM_IP || '10.152.59.219:8080',
  J3: process.env.J3_CAM_IP || '10.152.59.31:8080',
  MOBILE_CAM: process.env.MOBILE_CAM_IP || '10.152.59.144:8080'
};

// YOLOv8 COCO class names (80 standard)
const COCO_CLASSES = [
  'person','bicycle','car','motorcycle','airplane','bus','train','truck',
  'boat','traffic light','fire hydrant','stop sign','parking meter','bench',
  'bird','cat','dog','horse','sheep','cow','elephant','bear','zebra','giraffe',
  'backpack','umbrella','handbag','tie','suitcase','frisbee','skis','snowboard',
  'sports ball','kite','baseball bat','baseball glove','skateboard','surfboard',
  'tennis racket','bottle','wine glass','cup','fork','knife','spoon','bowl',
  'banana','apple','sandwich','orange','broccoli','carrot','hot dog','pizza',
  'donut','cake','chair','couch','potted plant','bed','dining table','toilet',
  'tv','laptop','mouse','remote','keyboard','cell phone','microwave','oven',
  'toaster','sink','refrigerator','book','clock','vase','scissors',
  'teddy bear','hair drier','toothbrush',
];

const VEHICLE_CLASSES = new Set([
  'car','truck','bus','motorcycle','bicycle','ambulance','fire_truck','firetruck','train',
  'rickshaw', 'cng', 'police', 'police-car', 'van', 'autorickshaw',
  '0', '1', '2', '3', '4', '5' // Numeric labels found in v3 dataset (0,1=car, 2,3=truck, 4,5=bus)
]);
const PERSON_CLASSES    = new Set(['person']);
// Specific triggers for Emergency Authority (v3 labels and IDs included)
const EMERGENCY_CLASSES = new Set([
  'ambulance', 'firetruck', 'police', 'police-car', 'fire brigade',
  '10', '11', '21', '22', '23' // Numeric IDs potentially used for Emergency in v3
]); 

// ─────────────────────────────────────────────────────────────
//  Global state  (one entry per junction)
// ─────────────────────────────────────────────────────────────
const state = {};
for (const [jct, ip] of Object.entries(JUNCTIONS)) {
  state[jct] = {
    vehicles: 0, persons: 0, traffic_score: 0,
    ambulance: false, fire_truck: false, accident: false,
    signal: 'GREEN', emergency: false, manual: false,
    dist: 999, ir: 0, pir: 0,
    cam_online: false, last_update: null,
    cam_url: `http://${ip}/capture`,
    detections: [],
    active: true,
    ai_signal: 'UNKNOWN',
    _lastRFCall: 0,
    _lastEmBroadcast: 0,
    _lastAccidentAlert: 0,
    _isVirtual: false,
    _history: [] // Rolling chart data (v, t)
  };
}



function updateHistory(jct) {
  const jst = state[jct];
  jst._history.push({ v: jst.vehicles, t: jst.traffic_score, ts: Date.now() });
  if (jst._history.length > 20) jst._history.shift();
}

let lastGridlockCheck = 0;
let lastSignalOrchestration = 0;
let currentGreenJunction = 'J1'; // Track which junction is currently green
let lastJunctionSwitch = 0;
const MIN_GREEN_MS = 10000; // 10 seconds minimum per junction turn

function runCityIntelligence() {
  const now = Date.now();
  orchestrateTrafficSignals(now); 

  if (now - lastGridlockCheck < 10000) return;
  lastGridlockCheck = now;

  const heavy = Object.keys(state).filter(j => state[j].traffic_score > 85);
  if (heavy.length >= 2) {
    addAlert({
      source: 'CITY_CORE',
      type: 'GRIDLOCK',
      details: `Critical load across nodes: ${heavy.join(', ')}. Diverting traffic flow.`
    });
  }
}

/** 🚦 AI Cognitive Orchestrator: Decision Logic Based on Image Datasets */
function orchestrateTrafficSignals(now) {
  if (now - lastSignalOrchestration < 2000) return;
  lastSignalOrchestration = now;

  const nodes = ['J1', 'J2', 'J3']; // Primary nodes to balance
  const jStates = nodes.map(j => ({ id: j, ...state[j] }));

  // 1. Emergency Priority (Hard Interrupt)
  const emNode = jStates.find(s => s.ambulance || s.fire_truck || s.emergency);
  if (emNode) {
    if (currentGreenJunction !== emNode.id) {
       switchSignal(emNode.id, 'EMERGENCY_PRIORITY');
    }
    return;
  }

  // 2. Minimum Turn Lock (Prevent flickering)
  if (now - lastJunctionSwitch < MIN_GREEN_MS) return;

  // 3. Demand-Based Selection (Weighting Traffic Score)
  const highestDemand = jStates.sort((a, b) => b.traffic_score - a.traffic_score)[0];
  
  // Only switch if substantial difference or current is clear
  const currentScore = state[currentGreenJunction].traffic_score;
  const shouldSwitch = (highestDemand.traffic_score > currentScore + 15) || (currentScore < 5);

  if (shouldSwitch && highestDemand.id !== currentGreenJunction) {
    switchSignal(highestDemand.id, `DEMAND_LOAD_${highestDemand.traffic_score}%`);
  }
}

function switchSignal(targetJct, reason) {
  const nodes = ['J1', 'J2', 'J3'];
  console.log(`[AI Orchestrator] Target: ${targetJct} | Reason: ${reason} | Executing city-wide sync.`);
  
  nodes.forEach(j => {
    const cmd = (j === targetJct) ? 'GREEN' : 'RED';
    mqttPublish(`smartjunction/${j}/cmd`, cmd);
    state[j].signal = cmd; // Optimistic status update
    state[j].last_update = new Date().toISOString();
  });

  currentGreenJunction = targetJct;
  lastJunctionSwitch = Date.now();
  io.emit('state_update', publicState());
  
  addAlert({
    source: 'AI_ORCHESTRATOR',
    type: 'SIGNAL_SYNC',
    details: `Switching focus to ${targetJct} based on ${reason}`
  });
}

/** Rolling alert log (newest first, capped at 200) */
const alerts = [];
let alertsTotal = 0;

// ─────────────────────────────────────────────────────────────
//  YOLO model  (loaded once at startup)
// ─────────────────────────────────────────────────────────────
let session = null;

async function loadYOLO() {
  if (!fs.existsSync(YOLO_MODEL)) {
    console.warn('[YOLO] ⚠  Model not found:', YOLO_MODEL);
    console.warn('[YOLO]    Run: node download-model.js');
    console.warn('[YOLO]    Falling back to heuristic-only mode.');
    return;
  }
  try {
    session = await ort.InferenceSession.create(YOLO_MODEL, {
      executionProviders: ['cpu'],
      graphOptimizationLevel: 'all',
    });
    console.log('[YOLO] ✓ Model loaded:', YOLO_MODEL);
  } catch (err) {
    console.error('[YOLO] Load failed:', err.message);
    session = null;
  }
}

// ─────────────────────────────────────────────────────────────
//  Image pre-processing  JPEG Buffer → ONNX Float32 tensor
//  YOLOv8 expects: [1, 3, 640, 640]  float32  0–1 normalised
// ─────────────────────────────────────────────────────────────
const YOLO_SIZE = 640;

async function jpegToTensor(jpegBuf) {
  // Force 3-channel RGB output (removes alpha if present)
  const { data } = await sharp(jpegBuf)
    .resize(YOLO_SIZE, YOLO_SIZE)
    .removeAlpha()           // ← ensure exactly 3 channels
    .toFormat('raw')
    .toBuffer({ resolveWithObject: true });

  const float32  = new Float32Array(3 * YOLO_SIZE * YOLO_SIZE);
  const pixCount = YOLO_SIZE * YOLO_SIZE;
  for (let i = 0; i < pixCount; i++) {
    float32[i]              = data[i * 3]     / 255;  // R
    float32[i + pixCount]   = data[i * 3 + 1] / 255;  // G
    float32[i + pixCount*2] = data[i * 3 + 2] / 255;  // B
  }
  return new ort.Tensor('float32', float32, [1, 3, YOLO_SIZE, YOLO_SIZE]);
}

// ─────────────────────────────────────────────────────────────
//  YOLOv8 post-processing
//  Output tensor shape: [1, 84, num_boxes]  (ultralytics ONNX export)
//  Rows 0-3: cx, cy, w, h  |  Rows 4-83: class confidence scores
// ─────────────────────────────────────────────────────────────
function processYOLOOutput(rawOutput, imgW, imgH) {
  // rawOutput.dims = [1, 84, 8400]
  const [, rows, boxes] = rawOutput.dims;
  const data       = rawOutput.data;
  const numClasses = rows - 4;
  const detections = [];

  for (let b = 0; b < boxes; b++) {
    let maxScore = 0;
    let maxClass = 0;
    for (let c = 0; c < numClasses; c++) {
      const score = data[(4 + c) * boxes + b];
      if (score > maxScore) { maxScore = score; maxClass = c; }
    }
    if (maxScore < CONF_THRESH) continue;

    const cx = data[0 * boxes + b];
    const cy = data[1 * boxes + b];
    const w  = data[2 * boxes + b];
    const h  = data[3 * boxes + b];

    const scaleX = imgW / 640; // Local YOLO usually training on 640x640
    const scaleY = imgH / 640;
    const x1 = Math.max(0, Math.round((cx - w / 2) * scaleX));
    const y1 = Math.max(0, Math.round((cy - h / 2) * scaleY));
    const x2 = Math.min(imgW, Math.round((cx + w / 2) * scaleX));
    const y2 = Math.min(imgH, Math.round((cy + h / 2) * scaleY));

    const className = COCO_CLASSES[maxClass] || `cls_${maxClass}`;
    detections.push({
      class: className,
      conf: Math.round(maxScore * 1000) / 1000,
      bbox: [x1, y1, x2, y2],
    });
  }

  return nms(detections, 0.45);
}

// ─────────────────────────────────────────────────────────────
//  Roboflow Inference Logic
// ─────────────────────────────────────────────────────────────
async function runRoboflowInference(jpegBuf) {
  if (!RF_API_KEY) return [];
  
  try {
    const base64 = jpegBuf.toString('base64');
    const resp = await axios({
      method: 'POST',
      url: RF_URL,
      data: base64,
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      timeout: 8000
    });
    
    if (!resp.data.predictions) return [];

    console.log(`[Roboflow] ✓ Comparison Success: Validated against Dataset [${RF_MODEL_ID}]`);
    return resp.data.predictions.map(p => {
      // Roboflow center coords to x1,y1,x2,y2
      const x1 = Math.round(p.x - p.width / 2);
      const y1 = Math.round(p.y - p.height / 2);
      const x2 = Math.round(p.x + p.width / 2);
      const y2 = Math.round(p.y + p.height / 2);

      return {
        class: p.class.toLowerCase(),
        conf: p.confidence,
        bbox: [x1, y1, x2, y2]
      };
    });
  } catch (err) {
    console.error('[Roboflow] API Error:', err.message);
    return [];
  }
}

// IoU helper for NMS
function iou(a, b) {
  const ix1 = Math.max(a[0], b[0]), iy1 = Math.max(a[1], b[1]);
  const ix2 = Math.min(a[2], b[2]), iy2 = Math.min(a[3], b[3]);
  const iw   = Math.max(0, ix2 - ix1);
  const ih   = Math.max(0, iy2 - iy1);
  const inter = iw * ih;
  const aA = (a[2] - a[0]) * (a[3] - a[1]);
  const bA = (b[2] - b[0]) * (b[3] - b[1]);
  return inter / (aA + bA - inter + 1e-9);
}

// Greedy NMS (same class only)
function nms(dets, threshold) {
  dets.sort((a, b) => b.conf - a.conf);
  const keep       = [];
  const suppressed = new Array(dets.length).fill(false);
  for (let i = 0; i < dets.length; i++) {
    if (suppressed[i]) continue;
    keep.push(dets[i]);
    for (let j = i + 1; j < dets.length; j++) {
      if (
        !suppressed[j] &&
        dets[i].class === dets[j].class &&
        iou(dets[i].bbox, dets[j].bbox) > threshold
      ) { suppressed[j] = true; }
    }
  }
  return keep;
}

// ─────────────────────────────────────────────────────────────
//  Emergency colour heuristic
//  Checks for high red+blue pixel ratio inside a bounding box
//  → indicates flashing emergency lights on trucks/cars
// ─────────────────────────────────────────────────────────────
async function hasEmergencyLights(jpegBuf, bbox, imgW, imgH) {
  try {
    const [x1, y1, x2, y2] = bbox;
    const left   = Math.max(0, x1);
    const top    = Math.max(0, y1);
    const width  = Math.min(x2, imgW) - left;
    const height = Math.min(y2, imgH) - top;
    if (width <= 0 || height <= 0) return false;

    const { data } = await sharp(jpegBuf)
      .extract({ left, top, width, height })
      .removeAlpha()
      .toFormat('raw')
      .toBuffer({ resolveWithObject: true });

    let redPx = 0, bluePx = 0;
    const total = data.length / 3;
    for (let i = 0; i < data.length; i += 3) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      if (r > 160 && r > g * 1.5 && r > b * 1.5) redPx++;
      if (b > 160 && b > r * 1.5 && b > g * 1.5) bluePx++;
    }
    return (redPx + bluePx) / total > 0.10;
  } catch (_e) {
    return false;
  }
}

/** 🚦 AI-Vision Signal Color Detection (Chromatic Analysis) */
async function detectTrafficLightStatus(jpegBuf, bbox, imgW, imgH) {
  try {
    const [x1, y1, x2, y2] = bbox;
    const left   = Math.max(0, x1), top = Math.max(0, y1);
    const width  = Math.min(x2, imgW) - left, height = Math.min(y2, imgH) - top;
    if (width <= 5 || height <= 5) return 'UNKNOWN';

    const { data } = await sharp(jpegBuf)
      .extract({ left, top, width, height })
      .removeAlpha()
      .toFormat('raw')
      .toBuffer({ resolveWithObject: true });

    let rS = 0, gS = 0, yS = 0;
    const px = data.length / 3;
    for (let i = 0; i < data.length; i += 3) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      
      // Intelligent Spectral Density check (Brightness normalized)
      const brightness = (r + g + b) / 3;
      if (brightness < 40) continue; // Skip dark pixels

      if (r > 160 && r > g * 1.6 && r > b * 1.6) rS++; // Red
      else if (g > 160 && g > r * 1.3 && g > b * 1.1) gS++; // Green
      else if (r > 160 && g > 130 && b < 100) yS++; // Yellow
    }
    
    // Dynamic sensitivity threshold (1% of pixels minimum)
    const threshold = px * 0.01; 
    if (rS > gS && rS > yS && rS > threshold) return 'RED';
    if (gS > rS && gS > yS && gS > threshold) return 'GREEN';
    if (yS > rS && yS > gS && yS > threshold) return 'YELLOW';
    return 'UNKNOWN';
  } catch (_e) { return 'UNKNOWN'; }
}

// ─────────────────────────────────────────────────────────────
//  Accident heuristic
//  Fires when ≥ 3 persons AND ≥ 2 vehicles appear together
// ─────────────────────────────────────────────────────────────
function detectAccident(vehicleCount, personCount) {
  return personCount >= 3 && vehicleCount >= 2;
}

// ─────────────────────────────────────────────────────────────
//  Per-junction inference loop
// ─────────────────────────────────────────────────────────────
/**
 * General frame processing (called by IP puller or WebSocket pusher)
 */
async function processFrame(jct, jpegBuf) {
  if (!state[jct] || !state[jct].active) return;
  const jst = state[jct];

  // Get image dimensions for box scaling
  let imgW = 640, imgH = 480;
  // Performance optimization: skip metadata check if we already have it
  if (!jst._dim) {
    try {
      const meta = await sharp(jpegBuf).metadata();
      jst._dim = { w: meta.width || 640, h: meta.height || 480 };
    } catch (_e) { jst._dim = { w: 640, h: 480 }; }
  }
  imgW = jst._dim.w; imgH = jst._dim.h;

  let detections = [];

  // --- Hybrid Inference Engine ---
  const now = Date.now();
  const shouldCallRF = RF_API_KEY && (now - (jst._lastRFCall || 0) > RF_THROTTLE);

  // 1. Periodic Roboflow Validation (High Precision, User Dataset)
  if (shouldCallRF) {
    jst._lastRFCall = now;
    const rfDets = await runRoboflowInference(jpegBuf);
    if (rfDets && rfDets.length > 0) {
      detections = rfDets;
    }
  }

  // 2. Local YOLOv8 (Real-time, Low Latency tracking)
  if (detections.length === 0 && session) {
    try {
      const tensor  = await jpegToTensor(jpegBuf);
      const results = await session.run({ images: tensor });
      const output  = results[Object.keys(results)[0]];
      detections    = processYOLOOutput(output, imgW, imgH);
    } catch (err) {
      console.error(`[${jct}] Local inference fault:`, err.message);
    }
  }

  // Keep old detections if current inference fails (persistance)
  if (detections.length === 0 && jst.detections && jst.detections.length > 0) {
    detections = jst.detections;
  }

  // ── Count object classes ──────────────────────────────────
  let vehicleCount = 0, personCount = 0;
  let ambulance = false, fireTruck = false;

  for (const det of detections) {
    const cls = det.class.toLowerCase();
    
    // Classify Vehicle/Person
    if (VEHICLE_CLASSES.has(cls)) vehicleCount++;
    if (PERSON_CLASSES.has(cls))  personCount++;

    // High-Priority Emergency Signals (Roboflow v3 Direct Matches)
    if (EMERGENCY_CLASSES.has(cls)) {
      if (cls.includes('ambulance')) ambulance = true;
      if (cls.includes('fire') || cls.includes('truck') || cls.includes('police')) fireTruck = true;
    }

    // Secondary heuristic for generic COCO labels (Local YOLOv8 fallback)
    if (['truck', 'car', 'bus'].includes(cls) && !ambulance && !fireTruck) {
      const hasLights = await hasEmergencyLights(jpegBuf, det.bbox, imgW, imgH);
      if (hasLights) {
        if (cls === 'car') ambulance = true;
        else fireTruck = true;
      }
    }

    // 🚦 AI Signal Detection (Vision-based)
    if (cls === 'traffic light' || cls === 'traffic-light') {
       const detectedColor = await detectTrafficLightStatus(jpegBuf, det.bbox, imgW, imgH);
       if (detectedColor !== 'UNKNOWN') {
         det.signal = detectedColor;
         jst.ai_signal = detectedColor;
         console.log(`[AI Vision] ${jct} → Traffic Light status detected: ${detectedColor}`);
       }
    }
  }

  const trafficScore = Math.min(100, vehicleCount * 10);
  const accident     = detectAccident(vehicleCount, personCount);

  // ── Update junction state ─────────────────────────────────
  jst.vehicles     = vehicleCount;
  jst.persons      = personCount;
  jst.traffic_score = trafficScore;
  jst.ambulance    = ambulance;
  jst.fire_truck   = fireTruck;
  jst.accident     = accident;
  jst.detections   = detections.slice(0, 20);
  jst.last_update  = new Date().toISOString();
  jst._lastJpeg    = jpegBuf; // Cache for the /api/frame endpoint

  // PREDICTION SUMMARY: Log high-priority node results for verification
  if (jct === 'MOBILE_CAM') {
    const summary = detections.map(d => d.class).join(', ');
    console.log(`[AI Prediction] ${jct}: Found ${vehicleCount} vehicles. [${summary || 'Clear'}]`);
    console.log(`[AI Decision]   Traffic Load: ${trafficScore}% | Emergency: ${ambulance || fireTruck}`);
  }

  // ── Publish YOLO vision results to ESP32 ─────────────────
  mqttPublish(`smartjunction/${jct}/vision`, {
    junction: jct,
    vehicles: vehicleCount,
    persons:  personCount,
    traffic_score: trafficScore,
    ambulance,
    fire_truck: fireTruck,
    accident,
    ai_signal: jst.ai_signal || 'OFF',
    ts: Date.now(),
  });

  // ── Emergency broadcast ───────────────────────────────────
  if (ambulance || fireTruck) {
    const now = Date.now();
    if (now - jst._lastEmBroadcast > EMERGENCY_BROADCAST_INTERVAL) {
      jst._lastEmBroadcast = now;
      const emType = ambulance ? 'AMBULANCE' : 'FIRE_TRUCK';
      mqttPublish('smartjunction/emergency', { source: jct, type: emType });
      addAlert({
        source: jct, type: emType,
        details: `Emergency vehicle detected at ${jct} — lane priority activated`,
      });
      // 🚨 INSTANT SYNC: Don't wait for internal cycles
      orchestrateTrafficSignals(now); 
    }
  } else {
    jst._lastEmBroadcast = 0;
  }

  // ── Accident alert ────────────────────────────────────────
  if (accident) {
    const now = Date.now();
    if (now - jst._lastAccidentAlert > 30000) {
      jst._lastAccidentAlert = now;
      addAlert({
        source: jct, type: 'ACCIDENT',
        details: `Accident/illegal activity — ${vehicleCount} vehicles, ${personCount} persons`,
      });
      mqttPublish('smartjunction/alerts', {
        source: jct, type: 'ACCIDENT',
        details: `Accident/suspicious activity detected at ${jct}`,
      });
    }
  } else {
    jst._lastAccidentAlert = 0;
  }

  // Push detections update (without the big image buffer to save bandwidth)
  io.emit('vision_update', {
    junction: jct,
    detections: jst.detections,
    v: vehicleCount,
    p: personCount,
    t: trafficScore,
    em: (ambulance || fireTruck),
    violation: jst.violation
  });

  io.emit('state_update', publicState());
}

async function runInference(jct, ip) {
  if (!state[jct].active) {
    state[jct].cam_online = false;
    return;
  }
  
  const now = Date.now();
  const lastUp = new Date(state[jct].last_update || 0).getTime();
  if (state[jct]._isVirtual && (now - lastUp < 5000)) return;

  const endpoints = [`http://${ip}/capture`, `http://${ip}/shot.jpg` ];
  
  for (const url of endpoints) {
    try {
      const resp = await axios.get(url, {
        responseType: 'arraybuffer',
        timeout: 800, // Very tight timeout for maximum speed
      });
      const jpegBuf = Buffer.from(resp.data);
      state[jct].cam_online = true;

      // ⚡ IMMEDIATE PUSH: Send frame to dashboard before inference starts
      io.emit('vision_update', {
        junction: jct,
        image: jpegBuf,
        v: state[jct].vehicles, // last known
        p: state[jct].persons,
        t: state[jct].traffic_score
      });

      // 🧠 BACKGROUND AI: Start inference without blocking the next frame capture
      setImmediate(() => {
        processFrame(jct, jpegBuf).catch(err => {
            console.error(`[AI Error ${jct}]`, err.message);
        });
      });

      return;
    } catch (err) {
      state[jct].cam_online = false;
    }
  }
}


// ─────────────────────────────────────────────────────────────
//  Start per-junction inference loops (staggered)
// ─────────────────────────────────────────────────────────────
function startInferenceLoops() {
  for (const [jct, ip] of Object.entries(JUNCTIONS)) {
    console.log(`[Inference] Starting loop → ${jct}  http://${ip}`);
    const tick = async () => {
      try {
        await runInference(jct, ip);
      } catch (err) {
        console.error(`[${jct}] Unhandled inference error:`, err.message);
      }
      setTimeout(tick, INFER_MS);
    };
    // Stagger each junction start by 300 ms to spread CPU load
    setTimeout(tick, 300 + Object.keys(JUNCTIONS).indexOf(jct) * 300);
  }
}

// ─────────────────────────────────────────────────────────────
//  MQTT Core: Distributed Embedded Broker
// ─────────────────────────────────────────────────────────────

// Start an internal broker if port is free — makes the server self-sustaining
function startEmbeddedBroker(port) {
  return new Promise((resolve) => {
    const brokerServer = net.createServer(aedes.handle);
    brokerServer.once('error', (err) => {
      if (err.code === 'EADDRINUSE') {
        console.log(`[Broker] External MQTT broker already active on port ${port} — using existing host.`);
      } else {
        console.error('[Broker] Failed to start:', err.message);
      }
      resolve(false);
    });

    brokerServer.listen(port, () => {
      console.log(`[Broker] ✓ Cognitive Core MQTT Hub active → Port ${port}`);
      
      // Monitor clients (for debugging)
      aedes.on('client', (client) => {
        // console.log(`[Mesh] Linked: ${client.id}`);
      });
      
      resolve(true);
    });
  });
}

// Global MQTT Client
let mqttClient;

async function initMQTT() {
  await startEmbeddedBroker(MQTT_PORT);

  // Forced IPv4 '127.0.0.1' — avoids Windows IPv6 'localhost' resolution delays & connack timeouts
  const targetHost = (MQTT_HOST === 'localhost' || MQTT_HOST === '::1') ? '127.0.0.1' : MQTT_HOST;
  
  mqttClient = mqttLib.connect(`mqtt://${targetHost}:${MQTT_PORT}`, {
    clientId:        'SmartJunction-EdgeServer',
    clean:           true,
    reconnectPeriod: 2000, 
    connectTimeout:  30000, // Very generous for local handshake
    keepalive:       60,
  });

  mqttClient.on('connect', () => {
    console.log(`[MQTT] ✓ Client linked → ${MQTT_HOST}:${MQTT_PORT}`);
    mqttClient.subscribe('smartjunction/+/sensors',   { qos: 0 });
    mqttClient.subscribe('smartjunction/+/camera',    { qos: 0 });
    mqttClient.subscribe('smartjunction/alerts',      { qos: 0 });
    mqttClient.subscribe('smartjunction/emergency',   { qos: 1 });
    console.log('[MQTT] Zero-lag mesh channel synchronized.');
  });

  mqttClient.on('reconnect', () => {
    // Only warn if we haven't connected after a while to reduce noise
    if (mqttClient.reconnectCount > 3) {
      console.warn('[MQTT] Retrying network handshake…');
    }
  });

  mqttClient.on('error', (err) => {
    if (err.code !== 'ECONNREFUSED') {
       console.error('[MQTT] Fault detected:', err.message);
    }
  });

  mqttClient.on('message', handleMQTTMessage);
}

function handleMQTTMessage(topic, message) {
  let data;
  try {
    data = JSON.parse(message.toString());
  } catch (_e) {
    data = { raw: message.toString() };
    return;
  }

  const parts = topic.split('/'); 

  // 1. Per-junction sensors/camera topics
  if (parts.length === 3) {
    const [, jct, kind] = parts;
    if (state[jct]) {
      if (kind === 'sensors') {
        state[jct].dist          = data.distance ?? data.dist ?? state[jct].dist;
        state[jct].vehicles      = data.vehicles ?? state[jct].vehicles;
        state[jct].traffic_score = data.traffic_score ?? data.cong ?? state[jct].traffic_score;
        state[jct].pir           = data.pir ?? state[jct].pir;
        state[jct].ir            = data.ir  ?? state[jct].ir;
        state[jct].signal        = data.signal ?? state[jct].signal;
        state[jct].uptime        = data.uptime ?? state[jct].uptime;
        state[jct].last_update   = new Date().toISOString();

        if (data.violation && !state[jct].violation) {
          addAlert({ source: jct, type: 'VIOLATION', details: 'Hardware sensors detected an illegal movement!' });
        }
        state[jct].violation = !!data.violation;

        updateHistory(jct);
        runCityIntelligence();
        io.emit('state_update', publicState());
      }
      
      if (kind === 'camera') {
        state[jct].cam_online = (data.status === 'online' || data.status === 'heartbeat');
        state[jct].cam_url    = data.stream_url || state[jct].cam_url;
        io.emit('state_update', publicState());
      }
    }
  }

  // 2. Global topics
  if (parts.length === 2) {
    const kind = parts[1];
    if (kind === 'alerts') {
      addAlert(data);
    } else if (kind === 'emergency') {
      if (data.type === 'CLEAR') {
        for (const s of Object.values(state)) {
          s.emergency = false;
          s.ambulance = false;
          s.fire_truck = false;
          s.violation = false;
          s._lastEmBroadcast = 0;
        }
        io.emit('state_update', publicState());
      } else {
        addAlert({ source: data.source || '?', type: data.type || 'EMERGENCY', details: data.details || '' });
      }
    }
  }
}

function mqttPublish(topic, payload, retain = false) {
  if (!mqttClient.connected) return;
  mqttClient.publish(
    topic,
    typeof payload === 'string' ? payload : JSON.stringify(payload),
    { retain, qos: 0 },
  );
}

// ─────────────────────────────────────────────────────────────
//  Alert helper
// ─────────────────────────────────────────────────────────────
function addAlert(entry) {
  const alert = { ts: new Date().toISOString(), ...entry };
  alerts.unshift(alert);
  if (alerts.length > 200) alerts.pop();
  alertsTotal++;
  io.emit('new_alert', alert);
  io.emit('state_update', publicState());
}

function clearAlerts() {
  alerts.length = 0;
  // Note: We don't reset alertsTotal here as it's a lifetime counter, 
  // but you can if desired. We'll leave it to show system activity.
  io.emit('alerts_history', []);
  io.emit('state_update', publicState());
}

// ─────────────────────────────────────────────────────────────
//  Public state (strips internal _* fields and detections array)
// ─────────────────────────────────────────────────────────────
function publicState() {
  const snap = {};
  for (const [jct, s] of Object.entries(state)) {
    const { detections, _lastEmBroadcast, _lastAccidentAlert, ...rest } = s;
    snap[jct] = rest;
  }
  snap.alerts_total = alertsTotal;
  return snap;
}

// ─────────────────────────────────────────────────────────────
//  Express + Socket.IO
// ─────────────────────────────────────────────────────────────
const app    = express();
const server = http.createServer(app);
const io     = new Server(server, {
  cors: { origin: '*', methods: ['GET', 'POST'] },
  maxHttpBufferSize: 1e8 // 100MB for pushed high-res frames
});

app.use(express.static(path.join(__dirname, '../frontend')));

app.get('/', (_req, res) => {
  res.sendFile(path.join(__dirname, '../frontend', 'index.html'));
});

// ── REST Endpoints ────────────────────────────────────────────

// Full system state
app.get('/api/state', (_req, res) => res.json(publicState()));

// Single junction state
app.get('/api/state/:jct', (req, res) => {
  const s = state[req.params.jct];
  if (!s) return res.status(404).json({ error: 'Unknown junction' });
  const { detections, _lastEmBroadcast, _lastAccidentAlert, ...rest } = s;
  res.json(rest);
});

// Detections for a junction (includes bbox data)
app.get('/api/detections/:jct', (req, res) => {
  const s = state[req.params.jct];
  if (!s) return res.status(404).json({ error: 'Unknown junction' });
  res.json({ junction: req.params.jct, detections: s.detections || [] });
});

// Alert log
app.get('/api/alerts', (_req, res) => res.json(alerts));

// Camera info (all junctions)
app.get('/api/cameras', (_req, res) => {
  const cams = {};
  for (const [jct, ip] of Object.entries(JUNCTIONS)) {
    cams[jct] = {
      ip,
      stream_url:  `http://${ip}/stream`,
      capture_url: `http://${ip}/capture`,
      info_url:    `http://${ip}/info`,
      online:      state[jct].cam_online,
    };
  }
  res.json(cams);
});

// Send command to a junction via MQTT
app.post('/api/cmd/:jct/:cmd', (req, res) => {
  const { jct, cmd } = req.params;
  const allowed = ['GREEN', 'RED', 'YELLOW', 'AUTO', 'ALERT_PATROL'];
  const upperCmd = cmd.toUpperCase();
  if (!allowed.includes(upperCmd)) {
    return res.status(400).json({ error: `Invalid command. Allowed: ${allowed.join(', ')}` });
  }
  if (!JUNCTIONS[jct]) {
    return res.status(404).json({ error: 'Unknown junction' });
  }
  mqttPublish(`smartjunction/${jct}/cmd`, upperCmd);
  console.log(`[CMD] ${jct} ← ${upperCmd}`);
  res.json({ ok: true, junction: jct, cmd: upperCmd });
});

// Toggle junction active/inactive
app.post('/api/toggle/:jct', (req, res) => {
  const { jct } = req.params;
  if (!state[jct]) return res.status(404).json({ error: 'Unknown junction' });
  
  state[jct].active = !state[jct].active;
  
  // Reset data if turned off
  if (!state[jct].active) {
    Object.assign(state[jct], {
      vehicles: 0, persons: 0, traffic_score: 0,
      ambulance: false, fire_truck: false, accident: false,
      signal: 'OFF', dist: 999, detections: [],
      cam_online: false
    });
  }

  io.emit('state_update', publicState());
  console.log(`[STATE] ${jct} active → ${state[jct].active}`);
  res.json({ ok: true, junction: jct, active: state[jct].active });
});

// Clear emergency across all junctions
app.post('/api/emergency/clear', (_req, res) => {
  mqttPublish('smartjunction/emergency', { source: 'SERVER', type: 'CLEAR' }, false);
  // Reset internal throttles
  for (const jst of Object.values(state)) {
    jst._lastEmBroadcast = 0;
    jst.ambulance  = false;
    jst.fire_truck = false;
    jst.emergency  = false;
  }
  io.emit('state_update', publicState());
  res.json({ ok: true });
});

// Proxy single JPEG frame from ESP32-CAM (avoids browser CORS issues)
app.get('/api/frame/:jct', async (req, res) => {
  const jctId = req.params.jct;
  const jst = state[jctId];
  if (!jst) return res.status(404).send('Not found');

  // serve the latest cached frame immediately if available
  if (jst._lastJpeg) {
     res.set('Content-Type', 'image/jpeg');
     res.set('Cache-Control', 'no-store');
     return res.send(jst._lastJpeg);
  }

  const ip = JUNCTIONS[jctId];
  if (!ip) return res.status(404).send('Not found');

  const endpoints = [`http://${ip}/capture`, `http://${ip}/shot.jpg` ];
  for (const url of endpoints) {
    try {
      const resp = await axios.get(url, { responseType: 'arraybuffer', timeout: 2000 });
      const jpeg = Buffer.from(resp.data);
      jst._lastJpeg = jpeg;
      res.set('Content-Type', 'image/jpeg');
      res.set('Cache-Control', 'no-store');
      return res.send(jpeg);
    } catch (_e) {
      // Continue to next endpoint
    }
  }

  // Final fallback: Return dark placeholder
  try {
    const placeholder = await sharp({
      create: { width: 320, height: 240, channels: 3, background: { r: 30, g: 35, b: 50 } },
    }).jpeg().toBuffer();
    res.set('Content-Type', 'image/jpeg');
    res.send(placeholder);
  } catch (_e) {
    res.status(502).send('Camera unavailable');
  }
});

// Proxy MJPEG live stream from ESP32-CAM
app.get('/api/stream/:jct', async (req, res) => {
  const ip = JUNCTIONS[req.params.jct];
  if (!ip) return res.status(404).send('Not found');
  try {
    const upstream = await axios.get(`http://${ip}/stream`, {
      responseType: 'stream',
      timeout:      15000,
    });
    const ct = upstream.headers['content-type'] ||
               'multipart/x-mixed-replace;boundary=frame';
    res.set('Content-Type', ct);
    res.set('Cache-Control', 'no-store');
    upstream.data.pipe(res);
    req.on('close', () => upstream.data.destroy());
  } catch (_e) {
    res.status(502).send('Camera stream unavailable');
  }
});

// ── Socket.IO events ─────────────────────────────────────────
io.on('connection', (socket) => {
  // Send full state snapshot on connect
  socket.emit('state_update',   publicState());
  socket.emit('alerts_history', alerts.slice(0, 50));

  // Dashboard → junction command override (Support individual or ALL)
  socket.on('cmd', ({ junction, cmd }) => {
    const targets = (junction === 'ALL') ? Object.keys(JUNCTIONS) : [junction];
    
    targets.forEach(jct => {
      if (JUNCTIONS[jct]) {
        mqttPublish(`smartjunction/${jct}/cmd`, String(cmd).toUpperCase());
        console.log(`[WS CMD] ${jct} ← ${cmd}`);
      }
    });
  });

  // Dashboard → toggle junction
  socket.on('toggle_junction', ({ junction }) => {
    if (junction && state[junction]) {
      state[junction].active = !state[junction].active;
      
      // Reset data if turned off
      if (!state[junction].active) {
        Object.assign(state[junction], {
          vehicles: 0, persons: 0, traffic_score: 0,
          ambulance: false, fire_truck: false, accident: false,
          signal: 'OFF', dist: 999, detections: [],
          cam_online: false
        });
      }

      io.emit('state_update', publicState());
      console.log(`[WS TOGGLE] ${junction} active → ${state[junction].active}`);
    }
  });

  // Dashboard → GLOBAL clear emergency
  socket.on('emergency_clear', () => {
    mqttPublish('smartjunction/emergency', { source: 'SERVER', type: 'CLEAR' });
    
    // Hard reset all emergency-related state on the server
    for (const jct of Object.keys(state)) {
      state[jct].emergency  = false;
      state[jct].ambulance  = false;
      state[jct].fire_truck = false;
      state[jct].accident   = false;
      state[jct]._lastEmBroadcast = 0;
    }
    
    io.emit('state_update', publicState());
    console.log('[WS GLOBAL] Emergency state reset for all nodes');
  });

  // Browser → Push frame (for Virtual Cam / Mobile Cam)
  socket.on('push_frame', async ({ junction, image }) => {
    if (!junction || !image || !state[junction]) return;
    
    try {
      // image is expected as base64 string
      const buf = Buffer.from(image.replace(/^data:image\/\w+;base64,/, ""), 'base64');
      state[junction].cam_online = true;
      state[junction]._isVirtual = true; // Mark as virtual so poller skips
      await processFrame(junction, buf);
    } catch (err) {
      console.error(`[WS PUSH] Error processing frame for ${junction}:`, err.message);
    }
    socket.on('clear_alerts', () => {
      clearAlerts();
      console.log('[WS] Alerts cleared by user');
    });
  });
});

// ─────────────────────────────────────────────────────────────
//  Boot sequence
// ─────────────────────────────────────────────────────────────
(async () => {
  // Discover LAN IP for mobile access
  const nets = os.networkInterfaces();
  let localIp = '127.0.0.1';
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        localIp = net.address;
        break;
      }
    }
  }

  console.log('='.repeat(60));
  console.log('  SmartJunction AI — Node.js Edge Server v2.1');
  console.log(`  Junctions : ${Object.keys(JUNCTIONS).join(', ')}`);
  console.log(`  MQTT      : ${MQTT_HOST}:${MQTT_PORT}`);
  console.log(`  Dashboard : http://localhost:${WEB_PORT}`);
  console.log(`  Mobile    : http://${localIp}:${WEB_PORT}`);
  console.log(`  YOLO      : ${YOLO_MODEL}`);
  console.log(`  Infer FPS : ${(1000 / INFER_MS).toFixed(1)} per camera`);
  console.log('='.repeat(60));

  await loadYOLO();

  server.listen(WEB_PORT, () => {
    console.log(`[Web] Dashboard ready → http://localhost:${WEB_PORT}`);
  });

  // Initialize MQTT mesh
  await initMQTT();

  // Wait 1 s for MQTT to connect before starting inference
  setTimeout(startInferenceLoops, 1000);
})();
