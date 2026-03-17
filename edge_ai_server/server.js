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

// ─────────────────────────────────────────────────────────────
//  Configuration  (also read from .env)
// ─────────────────────────────────────────────────────────────
const MQTT_HOST   = process.env.MQTT_HOST   || 'localhost';
const MQTT_PORT   = parseInt(process.env.MQTT_PORT   || '1883', 10);
const WEB_PORT    = parseInt(process.env.WEB_PORT    || '5000', 10);
const YOLO_MODEL  = process.env.YOLO_MODEL  || path.join(__dirname, 'yolov8n.onnx');
const CONF_THRESH = parseFloat(process.env.CONF_THRESH || '0.45');
const INFER_MS    = Math.round(1000 / parseFloat(process.env.INFER_FPS || '2'));

// How often (ms) to re-broadcast emergency per junction (throttle)
const EMERGENCY_BROADCAST_INTERVAL = 10000;

// Junction ID → camera IP mapping
const JUNCTIONS = {
  J1: process.env.J1_CAM_IP || '192.168.1.101',
  J2: process.env.J2_CAM_IP || '192.168.1.102',
  J3: process.env.J3_CAM_IP || '192.168.1.103',
  J4: process.env.J4_CAM_IP || '192.168.1.104',
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
  'car','truck','bus','motorcycle','bicycle','ambulance','fire_truck','train',
]);
const PERSON_CLASSES    = new Set(['person']);
// EMERGENCY_CLASSES used in hasEmergencyLights heuristic branch
const EMERGENCY_CLASSES = new Set(['ambulance','fire_truck']); // eslint-disable-line no-unused-vars

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
    cam_url: `http://${ip}`,
    detections: [],
    active: true, // Only process if active
    // internal throttle: last time we broadcast an emergency for this junction
    _lastEmBroadcast: 0,
    // internal: last accident alert timestamp (prevent spam)
    _lastAccidentAlert: 0,
  };
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

    const scaleX = imgW / YOLO_SIZE;
    const scaleY = imgH / YOLO_SIZE;
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
async function runInference(jct, ip) {
  if (!state[jct].active) {
    state[jct].cam_online = false;
    return;
  }
  const captureUrl = `http://${ip}/capture`;
  let jpegBuf;

  try {
    const resp = await axios.get(captureUrl, {
      responseType: 'arraybuffer',
      timeout: 4000,
    });
    jpegBuf = Buffer.from(resp.data);
    state[jct].cam_online = true;
  } catch (_e) {
    state[jct].cam_online = false;
    return;
  }

  // Get image dimensions
  let imgW = 640, imgH = 480;
  try {
    const meta = await sharp(jpegBuf).metadata();
    imgW = meta.width  || imgW;
    imgH = meta.height || imgH;
  } catch (_e) { /* use defaults */ }

  let detections = [];

  // ── YOLO inference ────────────────────────────────────────
  if (session) {
    try {
      const tensor  = await jpegToTensor(jpegBuf);
      const results = await session.run({ images: tensor });
      const output  = results[Object.keys(results)[0]];
      detections    = processYOLOOutput(output, imgW, imgH);
    } catch (err) {
      console.error(`[${jct}] Inference error:`, err.message);
    }
  }

  // ── Count object classes ──────────────────────────────────
  let vehicleCount = 0, personCount = 0;
  let ambulance = false, fireTruck = false;

  for (const det of detections) {
    const cls = det.class.toLowerCase();
    if (VEHICLE_CLASSES.has(cls)) vehicleCount++;
    if (PERSON_CLASSES.has(cls))  personCount++;

    // Emergency vehicle detection via colour heuristic on large vehicles
    if (['truck', 'car', 'bus'].includes(cls)) {
      const hasLights = await hasEmergencyLights(jpegBuf, det.bbox, imgW, imgH);
      if (hasLights) {
        // We guess it's an ambulance if it's car-sized, fire truck if larger
        if (cls === 'car') ambulance = true;
        else fireTruck = true;
      }
    }
  }

  const trafficScore = Math.min(100, vehicleCount * 10);
  const accident     = detectAccident(vehicleCount, personCount);

  // ── Update junction state ─────────────────────────────────
  const jst        = state[jct];
  jst.vehicles     = vehicleCount;
  jst.persons      = personCount;
  jst.traffic_score = trafficScore;
  jst.ambulance    = ambulance;
  jst.fire_truck   = fireTruck;
  jst.accident     = accident;
  jst.detections   = detections.slice(0, 20);
  jst.last_update  = new Date().toISOString();

  // ── Publish YOLO vision results to ESP32 ─────────────────
  mqttPublish(`smartjunction/${jct}/vision`, {
    junction: jct,
    vehicles: vehicleCount,
    persons:  personCount,
    traffic_score: trafficScore,
    ambulance,
    fire_truck: fireTruck,
    accident,
    ts: Date.now(),
  });

  // ── Emergency broadcast (throttled per junction) ──────────
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
      console.warn(`[${jct}] 🚨 ${emType} detected!`);
    }
  } else {
    // Reset broadcast throttle when no emergency visible
    jst._lastEmBroadcast = 0;
  }

  // ── Accident / suspicious activity alert (throttled 30 s) ─
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
      console.warn(`[${jct}] ⚠️  Accident heuristic triggered`);
    }
  } else {
    jst._lastAccidentAlert = 0;
  }

  // Push live update to dashboard
  io.emit('state_update', publicState());
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
//  MQTT client
// ─────────────────────────────────────────────────────────────
const mqttClient = mqttLib.connect(`mqtt://${MQTT_HOST}:${MQTT_PORT}`, {
  clientId:        'SmartJunction-EdgeServer',
  clean:           true,
  reconnectPeriod: 3000,
  connectTimeout:  8000,
});

mqttClient.on('connect', () => {
  console.log(`[MQTT] ✓ Connected → ${MQTT_HOST}:${MQTT_PORT}`);
  mqttClient.subscribe('smartjunction/+/sensors',   { qos: 0 });
  mqttClient.subscribe('smartjunction/+/camera',    { qos: 0 });
  mqttClient.subscribe('smartjunction/alerts',      { qos: 0 });
  mqttClient.subscribe('smartjunction/emergency',   { qos: 1 });
  console.log('[MQTT] Subscribed to all smartjunction/# topics');
});

mqttClient.on('reconnect', () => {
  console.warn('[MQTT] Reconnecting…');
});

mqttClient.on('error', (err) => {
  console.error('[MQTT] Error:', err.message);
});

mqttClient.on('message', (topic, message) => {
  let data;
  try {
    data = JSON.parse(message.toString());
  } catch (_e) {
    data = { raw: message.toString() };
  }

  const parts = topic.split('/'); // e.g. ['smartjunction', 'J1', 'sensors']

  // ── Per-junction topics (length 3) ────────────────────────
  if (parts.length === 3) {
    const [, jct, kind] = parts;
    if (state[jct]) {
      if (kind === 'sensors') {
        Object.assign(state[jct], {
          dist:          data.dist      ?? state[jct].dist,
          ir:            data.ir        ?? state[jct].ir,
          pir:           data.pir       ?? state[jct].pir,
          vehicles:      data.vehicles  ?? state[jct].vehicles,
          persons:       data.persons   ?? state[jct].persons,
          traffic_score: data.traffic_score ?? state[jct].traffic_score,
          ambulance:     data.ambulance ?? (data.emergency && data.type === 'AMBULANCE') ?? state[jct].ambulance,
          fire_truck:    data.fire_truck ?? (data.emergency && data.type === 'FIRE_TRUCK') ?? state[jct].fire_truck,
          signal:        data.signal    ?? state[jct].signal,
          emergency:     data.emergency ?? state[jct].emergency,
          manual:        data.manual    ?? state[jct].manual,
          last_update:   new Date().toISOString(),
        });
        io.emit('state_update', publicState());
      }
      if (kind === 'camera') {
        state[jct].cam_online  = data.status === 'online' || data.status === 'heartbeat';
        state[jct].cam_url     = data.stream_url || state[jct].cam_url;
        state[jct].last_update = new Date().toISOString();
        io.emit('state_update', publicState());
      }
    }
  }

  // ── Broadcast topics (length 2) ───────────────────────────
  if (parts.length === 2) {
    const kind = parts[1];
    const src  = data.source || '?';
    const type = data.type   || 'INFO';
    if ((kind === 'alerts' || kind === 'emergency') && type !== 'CLEAR') {
      addAlert({ source: src, type, details: data.details || '' });
    }
  }
});

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

// ─────────────────────────────────────────────────────────────
//  Public state (strips internal _* fields and detections array)
// ─────────────────────────────────────────────────────────────
function publicState() {
  const snap = {};
  for (const [jct, s] of Object.entries(state)) {
    const { detections, _lastEmBroadcast, _lastAccidentAlert, ...rest } = s;
    snap[jct] = rest;
  }
  return snap;
}

// ─────────────────────────────────────────────────────────────
//  Express + Socket.IO
// ─────────────────────────────────────────────────────────────
const app    = express();
const server = http.createServer(app);
const io     = new Server(server, {
  cors: { origin: '*', methods: ['GET', 'POST'] },
  pingTimeout: 30000,
  pingInterval: 10000,
});

app.use(express.json());
app.use(express.urlencoded({ extended: false }));

// Serve the dashboard HTML
app.get('/', (_req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'dashboard.html'));
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
  const ip = JUNCTIONS[req.params.jct];
  if (!ip) return res.status(404).send('Not found');
  try {
    const resp = await axios.get(`http://${ip}/capture`, {
      responseType: 'arraybuffer',
      timeout:      3000,
    });
    res.set('Content-Type',  'image/jpeg');
    res.set('Cache-Control', 'no-store');
    res.send(Buffer.from(resp.data));
  } catch (_e) {
    // Return a dark grey placeholder JPEG when camera is offline
    try {
      const placeholder = await sharp({
        create: { width: 320, height: 240, channels: 3,
                  background: { r: 30, g: 35, b: 50 } },
      }).jpeg().toBuffer();
      res.set('Content-Type', 'image/jpeg');
      res.set('Cache-Control', 'no-store');
      res.send(placeholder);
    } catch (err2) {
      res.status(502).send('Camera unavailable');
    }
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
});

// ─────────────────────────────────────────────────────────────
//  Boot sequence
// ─────────────────────────────────────────────────────────────
(async () => {
  console.log('='.repeat(60));
  console.log('  SmartJunction AI — Node.js Edge Server v2.1');
  console.log(`  Junctions : ${Object.keys(JUNCTIONS).join(', ')}`);
  console.log(`  MQTT      : ${MQTT_HOST}:${MQTT_PORT}`);
  console.log(`  Dashboard : http://localhost:${WEB_PORT}`);
  console.log(`  YOLO      : ${YOLO_MODEL}`);
  console.log(`  Infer FPS : ${(1000 / INFER_MS).toFixed(1)} per camera`);
  console.log('='.repeat(60));

  await loadYOLO();

  server.listen(WEB_PORT, () => {
    console.log(`[Web] Dashboard ready → http://localhost:${WEB_PORT}`);
  });

  // Wait 2 s for MQTT to connect before starting inference
  setTimeout(startInferenceLoops, 2000);
})();
